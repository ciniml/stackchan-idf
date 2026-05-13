#include "eyebrow.hpp"

namespace stackchan::avatar::internal {

void draw_eyebrow(M5Canvas& canvas, const Eyebrow& eyebrow, const DrawContext& ctx,
                  std::int16_t breath_offset_y)
{
    const std::uint16_t fg = ctx.palette.primary;
    const float cx = static_cast<float>(eyebrow.center_x);
    const float cy = static_cast<float>(eyebrow.center_y) + static_cast<float>(breath_offset_y);
    const float w = static_cast<float>(eyebrow.width);
    const float h = static_cast<float>(eyebrow.height);

    if (ctx.expression == Expression::Angry || ctx.expression == Expression::Sad) {
        const float aspect = (eyebrow.is_left ^ (ctx.expression == Expression::Sad)) ? -1.0f : 1.0f;
        const float dx = aspect * 3.0f;
        const float dy = aspect * 5.0f;
        const float x1 = cx - w / 2.0f;
        const float x2 = x1 - dx;
        const float x4 = cx + w / 2.0f;
        const float x3 = x4 + dx;
        const float y1 = cy - h / 2.0f - dy;
        const float y2 = cy + h / 2.0f - dy;
        const float y3 = cy - h / 2.0f + dy;
        const float y4 = cy + h / 2.0f + dy;
        canvas.fillTriangle(static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y1),
                            static_cast<std::int16_t>(x2), static_cast<std::int16_t>(y2),
                            static_cast<std::int16_t>(x3), static_cast<std::int16_t>(y3), fg);
        canvas.fillTriangle(static_cast<std::int16_t>(x2), static_cast<std::int16_t>(y2),
                            static_cast<std::int16_t>(x3), static_cast<std::int16_t>(y3),
                            static_cast<std::int16_t>(x4), static_cast<std::int16_t>(y4), fg);
    } else {
        const float happy_offset = (ctx.expression == Expression::Happy) ? -5.0f : 0.0f;
        const float x = cx - w / 2.0f;
        const float y = cy - h / 2.0f + happy_offset;
        canvas.fillRect(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                        static_cast<std::int16_t>(eyebrow.width), static_cast<std::int16_t>(eyebrow.height), fg);
    }
}

} // namespace stackchan::avatar::internal
