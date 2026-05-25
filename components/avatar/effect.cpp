// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "effect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stackchan::avatar::internal {

namespace {

void draw_sweat(M5Canvas& canvas, std::int16_t x, std::int16_t y, float r, float offset, std::uint16_t color)
{
    const std::int16_t yy = static_cast<std::int16_t>(y + std::floor(offset * 5.0f));
    const float rr = r + std::floor(r * 0.2f * offset);
    const float a = std::sqrt(3.0f) * rr / 2.0f;
    canvas.fillCircle(x, yy, static_cast<std::int16_t>(std::round(rr)), color);
    canvas.fillTriangle(x, static_cast<std::int16_t>(yy - rr * 2.0f),
                        static_cast<std::int16_t>(x - a), static_cast<std::int16_t>(yy - rr * 0.5f),
                        static_cast<std::int16_t>(x + a), static_cast<std::int16_t>(yy - rr * 0.5f), color);
}

void draw_anger(M5Canvas& canvas, std::int16_t cx, std::int16_t cy, float r, float offset,
                std::uint16_t fg, std::uint16_t bg)
{
    const float rr = r + r * 0.4f * offset;
    // Outer cross
    canvas.fillRect(static_cast<std::int16_t>(cx - rr / 3.0f), static_cast<std::int16_t>(cy - rr),
                    static_cast<std::int16_t>(rr * 2.0f / 3.0f), static_cast<std::int16_t>(rr * 2.0f), fg);
    canvas.fillRect(static_cast<std::int16_t>(cx - rr), static_cast<std::int16_t>(cy - rr / 3.0f),
                    static_cast<std::int16_t>(rr * 2.0f), static_cast<std::int16_t>(rr * 2.0f / 3.0f), fg);
    // Inner cutout — hollow BOTH bars symmetrically so the mark reads as a
    // clean "#". (m5stack-avatar-rs only cuts the left third of the horizontal
    // bar — its width is r*2/3 instead of r*2 — leaving the right side a solid
    // block; we cut the full width here.)
    canvas.fillRect(static_cast<std::int16_t>(cx - rr / 3.0f + 2.0f), static_cast<std::int16_t>(cy - rr),
                    static_cast<std::int16_t>(rr * 2.0f / 3.0f - 4.0f), static_cast<std::int16_t>(rr * 2.0f), bg);
    canvas.fillRect(static_cast<std::int16_t>(cx - rr), static_cast<std::int16_t>(cy - rr / 3.0f + 2.0f),
                    static_cast<std::int16_t>(rr * 2.0f), static_cast<std::int16_t>(rr * 2.0f / 3.0f - 4.0f), bg);
}

void draw_chill(M5Canvas& canvas, std::int16_t x, std::int16_t y, float r, float offset, std::uint16_t color)
{
    const float h = r + std::fabs(r * 0.2f * offset);
    const float h_half = std::round(h / 2.0f);
    canvas.fillRect(static_cast<std::int16_t>(x - h_half), y, 3, static_cast<std::int16_t>(h_half), color);
    canvas.fillRect(x, y, 3, static_cast<std::int16_t>(h * 3.0f / 4.0f), color);
    canvas.fillRect(static_cast<std::int16_t>(x + h_half), y, 3, static_cast<std::int16_t>(h), color);
}

void draw_bubble(M5Canvas& canvas, std::int16_t x, std::int16_t y, float r, float offset, std::uint16_t color)
{
    const float rr = r + std::floor(r * 0.2f * offset);
    const float r_small = std::round(rr / 4.0f);
    canvas.fillCircle(x, y, static_cast<std::int16_t>(rr), color);
    canvas.fillCircle(x, y, static_cast<std::int16_t>(r_small), color);
}

void draw_heart(M5Canvas& canvas, std::int16_t x, std::int16_t y, float r, float offset,
                std::uint16_t color)
{
    const float rr = r + r * 0.4f * offset;
    const float a = rr * 1.41421356f / 4.0f; // r * sqrt(2) / 4
    const std::int16_t lobe_r = static_cast<std::int16_t>(std::round(rr / 2.0f));
    // Two top lobes.
    canvas.fillCircle(static_cast<std::int16_t>(x - rr / 2.0f), y, lobe_r, color);
    canvas.fillCircle(static_cast<std::int16_t>(x + rr / 2.0f), y, lobe_r, color);
    // Two triangles forming the lower point (share the wide base at y + a).
    canvas.fillTriangle(x, y,
                        static_cast<std::int16_t>(x - rr / 2.0f - a), static_cast<std::int16_t>(y + a),
                        static_cast<std::int16_t>(x + rr / 2.0f + a), static_cast<std::int16_t>(y + a),
                        color);
    canvas.fillTriangle(x, static_cast<std::int16_t>(y + rr / 2.0f + 2.0f * a),
                        static_cast<std::int16_t>(x - rr / 2.0f - a), static_cast<std::int16_t>(y + a),
                        static_cast<std::int16_t>(x + rr / 2.0f + a), static_cast<std::int16_t>(y + a),
                        color);
}

} // namespace

void draw_effect(M5Canvas& canvas, const DrawContext& ctx)
{
    const std::uint16_t fg = ctx.palette.primary;
    const std::uint16_t bg = ctx.palette.background;
    const float offset = ctx.breath;

    // The effect anchors are authored for the 320x240 layout (top-right of the
    // face). Scale + centre them the same way the face is scaled so they track
    // the avatar's top-right at any canvas size. For 320x240 this is identity.
    const float scale = std::min(static_cast<float>(canvas.width()) / 320.0f,
                                 static_cast<float>(canvas.height()) / 240.0f);
    const float ccx = canvas.width() / 2.0f;
    const float ccy = canvas.height() / 2.0f;
    auto tx = [&](float bx) { return static_cast<std::int16_t>(ccx + (bx - 160.0f) * scale); };
    auto ty = [&](float by) { return static_cast<std::int16_t>(ccy + (by - 120.0f) * scale); };
    auto sr = [&](float r) { return r * scale; };

    switch (ctx.expression) {
    case Expression::Happy:
        draw_heart(canvas, tx(280), ty(50), sr(12.0f), offset, fg);
        break;
    case Expression::Doubt:
        draw_sweat(canvas, tx(290), ty(110), sr(7.0f), offset, fg);
        break;
    case Expression::Angry:
        draw_anger(canvas, tx(280), ty(50), sr(12.0f), offset, fg, bg);
        break;
    case Expression::Sad:
        draw_chill(canvas, tx(270), ty(0), sr(30.0f), offset, fg);
        break;
    case Expression::Sleepy:
        draw_bubble(canvas, tx(290), ty(40), sr(10.0f), offset, fg);
        draw_bubble(canvas, tx(270), ty(52), sr(6.0f), offset, fg);
        break;
    default:
        break;
    }
}

} // namespace stackchan::avatar::internal
