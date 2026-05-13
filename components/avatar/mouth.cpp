#include "mouth.hpp"

namespace stackchan::avatar::internal {

void draw_mouth(M5Canvas& canvas, const Mouth& mouth, const DrawContext& ctx,
                std::int16_t breath_offset_y)
{
    const std::uint16_t fg = ctx.palette.primary;
    const float open = ctx.mouth_open_ratio;
    const std::uint16_t h = mouth.min_height +
                            static_cast<std::uint16_t>(static_cast<float>(mouth.max_height - mouth.min_height) * open);
    const std::uint16_t w = mouth.min_width +
                            static_cast<std::uint16_t>(static_cast<float>(mouth.max_width - mouth.min_width) * (1.0f - open));
    const std::int16_t x = mouth.center_x - static_cast<std::int16_t>(w / 2);
    const std::int16_t y = mouth.center_y - static_cast<std::int16_t>(h / 2) +
                           static_cast<std::int16_t>(ctx.breath * 2.0f) + breath_offset_y;
    canvas.fillRect(x, y, static_cast<std::int16_t>(w), static_cast<std::int16_t>(h), fg);
}

} // namespace stackchan::avatar::internal
