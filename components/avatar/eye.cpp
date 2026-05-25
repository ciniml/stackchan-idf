// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "eye.hpp"

#include <cmath>

namespace stackchan::avatar::internal {

void draw_eye(M5Canvas& canvas, const Eye& eye, const DrawContext& ctx, std::int16_t breath_offset_y)
{
    const std::uint16_t fg = ctx.palette.primary;
    const std::uint16_t bg = ctx.palette.background;

    const float cx = static_cast<float>(eye.center_x) + ctx.gaze_horizontal * 3.0f;
    const float cy = static_cast<float>(eye.center_y) + ctx.gaze_vertical * 3.0f + static_cast<float>(breath_offset_y);

    if (ctx.eye_open_ratio <= 0.0f) {
        // closed: short rectangle
        const float w = eye.radius * 2.0f;
        const float h = 4.0f;
        canvas.fillRect(static_cast<std::int16_t>(cx - eye.radius), static_cast<std::int16_t>(cy - 2.0f),
                        static_cast<std::int16_t>(w), static_cast<std::int16_t>(h), fg);
        return;
    }

    // base filled circle
    canvas.fillCircle(static_cast<std::int16_t>(cx), static_cast<std::int16_t>(cy),
                      static_cast<std::int16_t>(eye.radius), fg);

    switch (ctx.expression) {
    case Expression::Angry:
    case Expression::Sad: {
        // mask one upper corner with a background triangle
        const float r = eye.radius;
        const float x0 = cx - r;
        const float y0 = cy - r;
        const float x1 = cx + r;
        const float y1 = y0;
        const bool flip = eye.is_left ^ (ctx.expression == Expression::Angry);
        const float x2 = flip ? x0 : x1;
        const float y2 = y0 + r;
        canvas.fillTriangle(static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y0),
                            static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y1),
                            static_cast<std::int16_t>(x2), static_cast<std::int16_t>(y2), bg);
        break;
    }
    case Expression::Happy:
    case Expression::Sleepy: {
        const float r = eye.radius;
        const float w = r * 2.0f + 4.0f;
        const float h = r + 2.0f;
        const float y_offset = (ctx.expression == Expression::Happy) ? r : 0.0f;
        // half-mask the eye
        canvas.fillRect(static_cast<std::int16_t>(cx - r), static_cast<std::int16_t>(cy - r + y_offset),
                        static_cast<std::int16_t>(w), static_cast<std::int16_t>(h), bg);
        if (ctx.expression == Expression::Happy) {
            // Carve a background circle out of the centre so the remaining
            // top arc reads as a happy "^" eye (m5stack-avatar-rs draws this
            // circle in the mask/background colour, not the foreground).
            const float small_r = r / 1.5f;
            canvas.fillCircle(static_cast<std::int16_t>(cx), static_cast<std::int16_t>(cy),
                              static_cast<std::int16_t>(std::round(small_r)), bg);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace stackchan::avatar::internal
