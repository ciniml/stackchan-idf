#pragma once

#include <cstdint>

#include <M5GFX.h>

#include "avatar/draw_context.hpp"

namespace stackchan::avatar::internal {

struct Eyebrow {
    std::int16_t center_x;
    std::int16_t center_y;
    std::uint16_t width;
    std::uint16_t height;
    bool is_left;
};

void draw_eyebrow(M5Canvas& canvas, const Eyebrow& eyebrow, const DrawContext& ctx,
                  std::int16_t breath_offset_y);

} // namespace stackchan::avatar::internal
