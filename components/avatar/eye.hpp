#pragma once

#include <cstdint>

#include <M5GFX.h>

#include "avatar/draw_context.hpp"

namespace stackchan::avatar::internal {

struct Eye {
    std::int16_t center_x;
    std::int16_t center_y;
    float radius;
    bool is_left;
};

void draw_eye(M5Canvas& canvas, const Eye& eye, const DrawContext& ctx, std::int16_t breath_offset_y);

} // namespace stackchan::avatar::internal
