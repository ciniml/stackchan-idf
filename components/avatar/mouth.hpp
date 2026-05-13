#pragma once

#include <cstdint>

#include <M5GFX.h>

#include "avatar/draw_context.hpp"

namespace stackchan::avatar::internal {

struct Mouth {
    std::int16_t center_x;
    std::int16_t center_y;
    std::uint16_t min_width;
    std::uint16_t max_width;
    std::uint16_t min_height;
    std::uint16_t max_height;
};

void draw_mouth(M5Canvas& canvas, const Mouth& mouth, const DrawContext& ctx,
                std::int16_t breath_offset_y);

} // namespace stackchan::avatar::internal
