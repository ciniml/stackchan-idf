// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/io_expander_py32.hpp"

#include <M5Unified.h>
#include <esp_log.h>

namespace stackchan::board {

namespace {

constexpr std::uint8_t kRegVersion = 0x02;
constexpr std::uint8_t kRegGpioModeLow = 0x03;
constexpr std::uint8_t kRegGpioModeHigh = 0x04;
constexpr std::uint8_t kRegGpioOutLow = 0x05;
constexpr std::uint8_t kRegGpioOutHigh = 0x06;
constexpr std::uint8_t kRegGpioPullUpLow = 0x09;
constexpr std::uint8_t kRegGpioPullUpHigh = 0x0A;
constexpr std::uint8_t kRegGpioPullDownLow = 0x0B;
constexpr std::uint8_t kRegGpioPullDownHigh = 0x0C;
// LED strip on the M5 base. The PY32 owns the NeoPixel timing — the host
// just writes a count + RGB565 LE RAM and toggles the refresh bit.
constexpr std::uint8_t kRegLedCfg = 0x24;          // [5:0]=count, [6]=refresh
constexpr std::uint8_t kRegLedRamStart = 0x30;     // 32 LEDs × 2 B each
constexpr std::uint8_t kLedCfgRefreshBit = 1u << 6;
constexpr std::uint8_t kLedCfgCountMask = 0x3F;

} // namespace

tl::expected<Py32Expander, Error> Py32Expander::probe(std::uint8_t address)
{
    const std::uint8_t version = m5::In_I2C.readRegister8(address, kRegVersion, kI2cFreq);
    if (version == 0x00 || version == 0xFF) {
        return tl::unexpected{Error::ExpanderProbe};
    }
    return Py32Expander{address};
}

tl::expected<void, Error> Py32Expander::write_bit(std::uint8_t reg_l, std::uint8_t reg_h, std::uint8_t pin, bool value)
{
    const std::uint8_t reg = (pin < 8) ? reg_l : reg_h;
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << (pin & 0x7));

    const std::uint8_t current = m5::In_I2C.readRegister8(address_, reg, kI2cFreq);
    const std::uint8_t next = value ? static_cast<std::uint8_t>(current | mask) : static_cast<std::uint8_t>(current & ~mask);
    if (!m5::In_I2C.writeRegister8(address_, reg, next, kI2cFreq)) {
        return tl::unexpected{Error::ExpanderWrite};
    }
    return {};
}

tl::expected<void, Error> Py32Expander::set_direction(std::uint8_t pin, bool output)
{
    return write_bit(kRegGpioModeLow, kRegGpioModeHigh, pin, output);
}

tl::expected<void, Error> Py32Expander::set_pull_up(std::uint8_t pin, bool enable)
{
    // Clear pull-down first to avoid pull-up/down conflict, then set pull-up.
    if (auto r = write_bit(kRegGpioPullDownLow, kRegGpioPullDownHigh, pin, false); !r) {
        return r;
    }
    return write_bit(kRegGpioPullUpLow, kRegGpioPullUpHigh, pin, enable);
}

tl::expected<void, Error> Py32Expander::digital_write(std::uint8_t pin, bool level)
{
    return write_bit(kRegGpioOutLow, kRegGpioOutHigh, pin, level);
}

tl::expected<void, Error> Py32Expander::set_led_count(std::uint8_t count)
{
    if (count > kMaxLeds) count = kMaxLeds;
    const std::uint8_t masked = count & kLedCfgCountMask;
    if (!m5::In_I2C.writeRegister8(address_, kRegLedCfg, masked, kI2cFreq)) {
        return tl::unexpected{Error::ExpanderWrite};
    }
    // Cache so refresh_leds() can write `count | bit6` without re-reading.
    last_count_ = masked;
    return {};
}

tl::expected<void, Error>
Py32Expander::write_led_colors(const std::uint8_t* data, std::size_t count)
{
    if (data == nullptr || count == 0) return {};
    if (count > kMaxLeds) count = kMaxLeds;
    const std::size_t n_bytes = count * 2;
    // One burst write of count*2 bytes starting at REG_LED_RAM_START. Each LED
    // takes 2 bytes in RGB565 little-endian (= [lo, hi]); see
    // docs/py32_ioexpander.md §6.2 for the bit layout. The PY32 auto-increments
    // its internal pointer, so the bytes land at LED slots 0..count-1.
    if (!m5::In_I2C.writeRegister(address_, kRegLedRamStart, data,
                                  n_bytes, kI2cFreq)) {
        static int s_warn_throttle = 0;
        if ((s_warn_throttle++ & 31) == 0) {
            ESP_LOGW("py32", "led RAM write failed (count=%u)", (unsigned)count);
        }
        return tl::unexpected{Error::ExpanderWrite};
    }
    return {};
}

tl::expected<void, Error> Py32Expander::refresh_leds()
{
    // Single write of `count | bit6` — PY32 latches RAM → WS2812 line when
    // both the count (bits 0-5 matching N) and bit 6 are set in REG_LED_CFG,
    // then self-clears bit 6 after committing. Skipping the upstream BSP's
    // read-modify-write halves per-frame I2C activity (2 transactions:
    // RAM burst + this write), reducing contention with M5Unified's other
    // I2C consumers — see docs/known_issues.md §1 for the lgfx mutex race
    // this addresses.
    if (!m5::In_I2C.writeRegister8(address_, kRegLedCfg,
                                   last_count_ | kLedCfgRefreshBit, kI2cFreq)) {
        static int s_warn_throttle = 0;
        if ((s_warn_throttle++ & 31) == 0) {
            ESP_LOGW("py32", "led refresh write failed");
        }
        return tl::unexpected{Error::ExpanderWrite};
    }
    return {};
}

} // namespace stackchan::board
