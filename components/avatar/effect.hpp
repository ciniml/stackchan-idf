#pragma once

#include <M5GFX.h>

#include "avatar/draw_context.hpp"

namespace stackchan::avatar::internal {

void draw_effect(M5Canvas& canvas, const DrawContext& ctx);

} // namespace stackchan::avatar::internal
