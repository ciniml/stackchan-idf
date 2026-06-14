// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstdint>
#include <tl/expected.hpp>

#include "board/board.hpp"
#include "board/led_strip.hpp"

// Forward-declare the espressif/led_strip handle to keep the heavy IDF
// driver headers out of every translation unit that just includes Board.
typedef struct led_strip_t* led_strip_handle_t;

namespace stackchan::board {

// Stack-chan cat-ear NeoPixel chain: 9 × WS2812 on each ear, 18 total on a
// single data line out of GPIO9. Chain order is left ear (indices 0..8) then
// right ear (indices 9..17) — that's the assumption the animations under
// main/led_task.cpp run on. RMT-backed (espressif/led_strip managed
// component); the driver clocks bits at the 800 kHz WS2812 rate from a 10 MHz
// resolution channel.
class NekomimiLedStrip : public LedStrip {
public:
    static constexpr std::size_t kLedsPerEar = 9;
    static constexpr std::size_t kCount = kLedsPerEar * 2;  // 18 (L + R)
    static constexpr int kDataGpio = 9;

    NekomimiLedStrip() noexcept = default;
    ~NekomimiLedStrip() noexcept override;

    NekomimiLedStrip(const NekomimiLedStrip&) = delete;
    NekomimiLedStrip& operator=(const NekomimiLedStrip&) = delete;
    NekomimiLedStrip(NekomimiLedStrip&&) noexcept = delete;
    NekomimiLedStrip& operator=(NekomimiLedStrip&&) noexcept = delete;

    // Install the RMT channel + WS2812 encoder. Must be called once before
    // the first show().
    tl::expected<void, Error> begin() override;

    void clear() noexcept override;
    void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept override;
    void set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept override;

    tl::expected<void, Error> show() override;

    std::size_t size() const noexcept override { return kCount; }

private:
    led_strip_handle_t handle_{nullptr};
    // Local frame buffer in GRB byte order so the underlying espressif driver
    // can blit straight to the wire. (The driver also does the host-side
    // gamma if configured; we keep gamma off for predictable scale8 math in
    // led_task.)
    std::array<std::uint8_t, kCount * 3> buf_{};
};

} // namespace stackchan::board
