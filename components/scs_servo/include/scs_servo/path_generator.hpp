#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace stackchan::scs_servo {

// Trapezoidal-velocity path generator, ported from stackchan-rs::PathGenerator.
// Positions are in raw servo units (0..1023 for SCS0009). max_velocity and
// max_acceleration are in units per tick (caller picks tick period).
template <std::size_t MaxWaypoints = 256>
class PathGenerator {
public:
    PathGenerator(std::uint32_t initial_position, float max_acceleration, float max_velocity) noexcept
        : current_{initial_position},
          target_{initial_position},
          max_acceleration_{max_acceleration},
          max_velocity_{max_velocity}
    {
    }

    bool is_moving() const noexcept { return current_ != target_; }
    std::uint32_t target() const noexcept { return target_; }
    std::uint32_t current() const noexcept { return current_; }
    void force_stop() noexcept { target_ = current_; }

    void begin_move_to(std::uint32_t target_position) noexcept
    {
        const int direction = (target_position < current_) ? -1 : 1;
        const std::uint32_t distance =
            (direction < 0) ? (current_ - target_position) : (target_position - current_);

        std::uint32_t steps =
            static_cast<std::uint32_t>(std::ceil(static_cast<float>(distance) / max_velocity_));
        std::uint32_t steps_to_accelerate =
            static_cast<std::uint32_t>(std::ceil(max_velocity_ / max_acceleration_));

        float effective_velocity;
        if (steps < steps_to_accelerate * 2) {
            effective_velocity = max_acceleration_ * static_cast<float>(steps) / 2.0f;
            steps_to_accelerate = steps / 2;
        } else {
            effective_velocity = max_velocity_;
        }

        steps = static_cast<std::uint32_t>(
            std::ceil(static_cast<float>(distance) / effective_velocity +
                      static_cast<float>(steps_to_accelerate)));
        if (steps > MaxWaypoints) {
            steps = MaxWaypoints;
        }

        for (std::uint32_t step = 0; step < steps; ++step) {
            std::uint32_t offset;
            if (step < steps_to_accelerate) {
                offset = static_cast<std::uint32_t>(
                    std::floor(static_cast<float>(step * step) * max_acceleration_ / 2.0f));
            } else if (step < steps - steps_to_accelerate) {
                const float base = static_cast<float>(steps_to_accelerate * steps_to_accelerate) *
                                   max_acceleration_ / 2.0f;
                offset = static_cast<std::uint32_t>(
                    std::ceil(base + effective_velocity *
                                         static_cast<float>(step - steps_to_accelerate)));
            } else {
                const std::uint32_t remaining = steps - step;
                offset = distance - static_cast<std::uint32_t>(std::floor(
                                        static_cast<float>(remaining * remaining) *
                                        max_acceleration_ / 2.0f));
            }
            waypoints_[step] = (direction < 0) ? (current_ - offset) : (current_ + offset);
        }

        way_point_ = 0;
        steps_ = static_cast<std::size_t>(steps);
        target_ = target_position;
    }

    std::uint32_t step_next() noexcept
    {
        if (way_point_ >= steps_) {
            current_ = target_;
            return target_;
        }
        const std::uint32_t position = waypoints_[way_point_++];
        current_ = position;
        return position;
    }

private:
    std::uint32_t current_;
    std::uint32_t target_;
    std::size_t way_point_{0};
    std::size_t steps_{0};
    std::array<std::uint32_t, MaxWaypoints> waypoints_{};
    float max_acceleration_;
    float max_velocity_;
};

} // namespace stackchan::scs_servo
