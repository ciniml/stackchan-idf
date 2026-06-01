// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/led_strip.hpp"

namespace stackchan::board {

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
    const std::uint16_t c = pack565(r, g, b);
    for (std::size_t i = 0; i < count_; ++i) buf_[i] = c;
}

void LedStrip::set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    if (index >= count_) return;
    buf_[index] = pack565(r, g, b);
}

tl::expected<void, Error> LedStrip::show()
{
    if (auto r = expander_->write_led_colors(buf_.data(), count_); !r) return r;
    return expander_->refresh_leds();
}

} // namespace stackchan::board
