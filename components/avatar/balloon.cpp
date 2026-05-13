#include "balloon.hpp"

namespace stackchan::avatar::internal {

void draw_balloon(M5Canvas& canvas, const DrawContext& ctx)
{
    if (!ctx.balloon_text.has_value()) {
        return;
    }
    const auto& text = *ctx.balloon_text;
    if (text.empty()) {
        return;
    }

    const std::uint16_t fg = ctx.palette.balloon_foreground;

    constexpr std::int16_t cx = 240;
    constexpr std::int16_t cy = 220;
    constexpr std::int16_t char_w = 6;
    constexpr std::int16_t char_h = 9;

    const std::int16_t text_w = static_cast<std::int16_t>(text.size()) * char_w;
    const std::int16_t outer_w = text_w + 12;
    const std::int16_t outer_h = char_h * 2 + 2;

    // Ellipse outline (M5GFX has drawEllipse(cx, cy, rx, ry, color))
    canvas.drawEllipse(cx - 20, cy, outer_w / 2, outer_h / 2, fg);
    // Tail (simple triangle outline drawn with 3 lines).
    canvas.drawLine(cx - 62, cy - 42, cx - 8, cy - 10, fg);
    canvas.drawLine(cx - 8, cy - 10, cx - 41, cy - 8, fg);
    canvas.drawLine(cx - 41, cy - 8, cx - 62, cy - 42, fg);

    canvas.setTextColor(fg);
    canvas.setTextSize(1);
    canvas.setCursor(static_cast<std::int16_t>(cx - text_w / 2 - 20), static_cast<std::int16_t>(cy - char_h / 2));
    canvas.print(text.c_str());
}

} // namespace stackchan::avatar::internal
