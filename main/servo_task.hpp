#pragma once

#include "shared_state.hpp"

namespace stackchan::app {

struct ServoTaskArgs {
    SharedState* state;
};

// Pinned to core 0, 20 ms period.
void start_servo_task(ServoTaskArgs& args);

} // namespace stackchan::app
