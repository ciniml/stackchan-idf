// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/led_strip.hpp"

namespace stackchan::board {

namespace {

// Pack (r,g,b) into the 2-byte RGB565 LE pair the PY32 LED RAM expects.
// Matches the upstream BSP packing (((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3)).
inline void pack_rgb565_le(std::uint8_t* out, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    const std::uint16_t v = Py32Expander::rgb565(r, g, b);
    out[0] = static_cast<std::uint8_t>(v & 0xFF);       // lo: GGG BBBBB
    out[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF); // hi: RRRRR GGG
}

} // namespace

tl::expected<void, Error> LedStrip::begin()
{
    if (auto r = expander_->set_led_count(count_); !r) return r;
    clear();
    return show();
}

void LedStrip::clear() noexcept
{
    buf_.fill(0);
}

void LedStrip::fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    for (std::size_t i = 0; i < count_; ++i) {
        pack_rgb565_le(&buf_[i * 2], r, g, b);
    }
}

void LedStrip::set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    if (index >= count_) return;
    pack_rgb565_le(&buf_[index * 2], r, g, b);
}

tl::expected<void, Error> LedStrip::show()
{
    if (auto r = expander_->write_led_colors(buf_.data(), count_); !r) return r;
    return expander_->refresh_leds();
}

} // namespace stackchan::board
