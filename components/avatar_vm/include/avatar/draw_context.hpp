// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "avatar/expression.hpp"
#include "avatar/palette.hpp"

namespace stackchan::avatar {

struct DrawContext {
    Expression expression{Expression::Neutral};
    float breath{0.0f};
    // External / commanded gaze target — written by Avatar::set_gaze (e.g.
    // touch-follow, future API gesture). Saccade is added on top so the
    // eyes still wander while pointing roughly at the target.
    float gaze_horizontal{0.0f};
    float gaze_vertical{0.0f};
    // Animator-driven saccade offset, summed with the target above by
    // Var::GazeH / Var::GazeV in the VM. Updated by FaceAnimator::
    // saccade_tick; kept zero when saccade is disabled.
    float gaze_saccade_h{0.0f};
    float gaze_saccade_v{0.0f};
    float eye_open_ratio{1.0f};
    float mouth_open_ratio{0.0f};
    Palette palette{kDefaultPalette};
    std::uint32_t rng_state{0xC0FFEEu};
    std::optional<std::string> balloon_text{};
    // Wall-clock used for time-based animation (e.g. balloon scroll).
    std::uint32_t now_ms{0};
    // Set to `now_ms` whenever balloon_text changes — drives the scroll phase.
    std::uint32_t balloon_set_ms{0};
    // Display time. 0 means "use balloon defaults" (fitting text = a fixed
    // hold, overflowing text = one scroll at a fixed speed); otherwise the
    // scroll of overflowing text is timed to finish within it.
    std::uint32_t balloon_hold_ms{0};
    // Set by balloon rendering once the message has been displayed in full
    // (i.e. hold time elapsed for short text, or the scroll finished for long
    // text). The render task polls this and notifies the application.
    bool balloon_done{false};
};

} // namespace stackchan::avatar
