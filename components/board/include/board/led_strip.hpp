// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstdint>
#include <tl/expected.hpp>

#include "board/board.hpp"
#include "board/io_expander_py32.hpp"

namespace stackchan::board {

// NeoPixel strip on the M5 Stack-chan base (12 × WS2812 on the back). The PY32
// MCU on the base handles the WS2812 timing; this class is a thin host-side
// frame buffer + I2C burst writer. A single show() pushes the whole buffer to
// PY32 RAM in one transaction and latches it onto the strip with a refresh.
// Not present on the Takao base — Board::led_strip() returns nullptr there.
class LedStrip {
public:
    LedStrip(Py32Expander& expander, std::uint8_t count) noexcept
        : expander_{&expander}, count_{count}
    {
    }

    // Push the strip count to the PY32 and clear the local frame buffer +
    // strip. Call once before the first show().
    tl::expected<void, Error> begin();

    // Local-buffer writes. None of these touch the bus; show() pushes the
    // whole buffer in one I2C burst.
    void clear() noexcept;
    void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;
    void set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

    // Push the local buffer to the strip + latch.
    tl::expected<void, Error> show();

    std::size_t size() const noexcept { return count_; }

private:
    Py32Expander* expander_;
    std::uint8_t count_;
    // 2 bytes per LED stored as RGB565 little-endian (= [lo, hi]). The PY32
    // firmware itself converts to WS2812 GRB on the wire; the host stays in
    // 565 LE. See docs/py32_ioexpander.md §6 for the bit layout.
    std::array<std::uint8_t, Py32Expander::kMaxLeds * 2> buf_{};
};

} // namespace stackchan::board
