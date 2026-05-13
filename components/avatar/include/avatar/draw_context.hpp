#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "avatar/expression.hpp"
#include "avatar/palette.hpp"

namespace stackchan::avatar {

struct DrawContext {
    Expression expression{Expression::Neutral};
    float breath{0.0f};
    float gaze_horizontal{0.0f};
    float gaze_vertical{0.0f};
    float eye_open_ratio{1.0f};
    float mouth_open_ratio{0.0f};
    Palette palette{kDefaultPalette};
    std::uint32_t rng_state{0xC0FFEEu};
    std::optional<std::string> balloon_text{};
};

} // namespace stackchan::avatar
