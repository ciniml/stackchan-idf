#include "effect.hpp"

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
    // Inner cutout
    canvas.fillRect(static_cast<std::int16_t>(cx - rr / 3.0f + 2.0f), static_cast<std::int16_t>(cy - rr),
                    static_cast<std::int16_t>(rr * 2.0f / 3.0f - 4.0f), static_cast<std::int16_t>(rr * 2.0f), bg);
    canvas.fillRect(static_cast<std::int16_t>(cx - rr), static_cast<std::int16_t>(cy - rr / 3.0f + 2.0f),
                    static_cast<std::int16_t>(rr * 2.0f / 3.0f), static_cast<std::int16_t>(rr * 2.0f / 3.0f - 4.0f), bg);
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

} // namespace

void draw_effect(M5Canvas& canvas, const DrawContext& ctx)
{
    const std::uint16_t fg = ctx.palette.primary;
    const std::uint16_t bg = ctx.palette.background;
    const float offset = ctx.breath;

    switch (ctx.expression) {
    case Expression::Doubt:
        draw_sweat(canvas, 290, 110, 7.0f, offset, fg);
        break;
    case Expression::Angry:
        draw_anger(canvas, 280, 50, 12.0f, offset, fg, bg);
        break;
    case Expression::Sad:
        draw_chill(canvas, 270, 0, 30.0f, offset, fg);
        break;
    case Expression::Sleepy:
        draw_bubble(canvas, 290, 40, 10.0f, offset, fg);
        draw_bubble(canvas, 270, 52, 6.0f, offset, fg);
        break;
    default:
        break;
    }
}

} // namespace stackchan::avatar::internal
