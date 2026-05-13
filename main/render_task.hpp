#pragma once

#include <M5GFX.h>

#include "shared_state.hpp"

namespace stackchan::app {

struct RenderTaskArgs {
    M5GFX* display;
    SharedState* state;
};

// Pinned to core 1, 33 ms period.
void start_render_task(RenderTaskArgs& args);

} // namespace stackchan::app
