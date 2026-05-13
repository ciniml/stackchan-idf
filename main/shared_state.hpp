#pragma once

#include <atomic>
#include <cstdint>

#include "avatar/expression.hpp"

namespace stackchan::app {

// Lock-free state shared between the render task and the servo task.
struct SharedState {
    std::atomic<float> target_yaw_deg{0.0f};
    std::atomic<float> target_pitch_deg{0.0f};
    std::atomic<float> mouth_open{0.0f};
    std::atomic<int> expression{static_cast<int>(avatar::Expression::Neutral)};
};

} // namespace stackchan::app
