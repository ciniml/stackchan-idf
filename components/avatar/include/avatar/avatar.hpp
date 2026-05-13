// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <M5GFX.h>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/palette.hpp"

namespace stackchan::avatar {

class Avatar {
public:
    explicit Avatar(M5GFX& display);
    ~Avatar();

    Avatar(const Avatar&) = delete;
    Avatar& operator=(const Avatar&) = delete;
    Avatar(Avatar&&) noexcept;
    Avatar& operator=(Avatar&&) noexcept;

    bool begin();

    void set_expression(Expression expression) noexcept;
    void set_mouth_open(float ratio) noexcept;
    void set_gaze(float horizontal, float vertical) noexcept;
    void set_palette(const Palette& palette) noexcept;
    // Show `text` in the balloon. `hold_ms` overrides the default display
    // time (0 = use balloon defaults: short text holds for a few seconds,
    // long text plays one full marquee pass).
    void set_balloon_text(std::string_view text, std::uint32_t hold_ms = 0);
    void clear_balloon() noexcept;
    // True once the current balloon has been fully displayed (hold elapsed
    // or one marquee pass completed). Stays true until the next
    // set_balloon_text / clear_balloon.
    bool is_balloon_done() const noexcept;

    // Drives animators with the current time in milliseconds and renders one frame.
    void tick(std::uint32_t now_ms);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stackchan::avatar
