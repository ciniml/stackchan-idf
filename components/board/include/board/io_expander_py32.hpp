// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <tl/expected.hpp>

#include "board/board.hpp"

namespace stackchan::board {

class Py32Expander {
public:
    static constexpr std::uint8_t kAddress = 0x6F;
    static constexpr std::uint32_t kI2cFreq = 100'000;

    static constexpr std::uint8_t kPinServoPowerEnable = 0;

    // M5 Stack-chan base mounts 12 WS2812 NeoPixels on the back, driven by
    // the PY32 MCU on its IO14 pin and exposed via I2C as a small RAM (RGB565
    // LE per LED) + a config register that holds the count + a refresh-trigger
    // bit. We never bit-bang the NeoPixel protocol from the ESP — the PY32
    // does it.
    static constexpr std::uint8_t kMaxLeds = 32;

    static tl::expected<Py32Expander, Error> probe(std::uint8_t address = kAddress);

    tl::expected<void, Error> set_direction(std::uint8_t pin, bool output);
    tl::expected<void, Error> set_pull_up(std::uint8_t pin, bool enable);
    tl::expected<void, Error> digital_write(std::uint8_t pin, bool level);

    // Set how many of the up-to-32 LEDs are active (the rest are ignored even
    // if data is written into their slots). Writes the count to REG_LED_CFG
    // [5:0], preserving the refresh bit.
    tl::expected<void, Error> set_led_count(std::uint8_t count);

    // Write the full strip in one I2C burst. `data` is count * 2 bytes, each
    // LED encoded as **RGB565 little-endian** = `[lo, hi]` where
    //   hi = RRRRR GGG       (R 5 bits, G upper 3 bits)
    //   lo = GGG BBBBB       (G lower 3 bits, B 5 bits)
    // i.e. a plain 16-bit RGB565 stored low-byte-first. The PY32 firmware
    // itself converts to WS2812 line order (GRB on the wire); the host stays
    // in RGB565. See docs/py32_ioexpander.md §6 for the bit layout.
    //
    // The call returns immediately after the I2C transaction — refresh_leds()
    // must be called separately to latch the buffer onto the strip.
    tl::expected<void, Error>
    write_led_colors(const std::uint8_t* data, std::size_t count);

    // Pack an RGB888 value into the 16-bit RGB565 the PY32 LED RAM expects.
    // Matches the upstream BSP's mapping (((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3)).
    static constexpr std::uint16_t rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        return static_cast<std::uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    // Toggle the refresh bit so the PY32 latches the RAM contents out onto
    // the NeoPixel wire. set_led_count / write_led_colors do not auto-refresh.
    //
    // Implementation detail: writes `last_count_ | bit6` directly (single I2C
    // transaction) instead of the upstream BSP's read-modify-write of
    // REG_LED_CFG. Skipping the read halves the per-frame I2C activity (2
    // transactions instead of 3 — RAM burst + refresh write — vs the BSP's
    // RAM burst + read + write) which materially reduces contention with
    // M5Unified's other I2C consumers and is what tipped the LCD-backlight
    // race in the past. Verified experimentally that PY32 commits RAM →
    // WS2812 line as long as the final CFG write has both `count` (bits 0-5
    // matching N) and `bit6` set — see `kLedRefreshExperiment` results.
    tl::expected<void, Error> refresh_leds();

    std::uint8_t address() const noexcept { return address_; }

private:
    explicit Py32Expander(std::uint8_t address) noexcept : address_{address} {}

    tl::expected<void, Error> write_bit(std::uint8_t reg_l, std::uint8_t reg_h, std::uint8_t pin, bool value);

    std::uint8_t address_;
    // Last successful set_led_count value, used by refresh_leds() to write
    // count + refresh-bit in one transaction (skips the RMW).
    std::uint8_t last_count_ = 0;
};

} // namespace stackchan::board
