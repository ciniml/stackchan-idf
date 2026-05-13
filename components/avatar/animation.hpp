#pragma once

#include <cstdint>

#include "avatar/draw_context.hpp"

namespace stackchan::avatar::internal {

// Simple xorshift32 RNG so we don't pull in <random>.
class XorShift32 {
public:
    explicit XorShift32(std::uint32_t seed) noexcept : state_{seed != 0 ? seed : 0x12345678u} {}

    std::uint32_t next() noexcept
    {
        std::uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }

    float next_range(float low, float high) noexcept
    {
        return low + (high - low) * (static_cast<float>(next()) / static_cast<float>(0xFFFFFFFFu));
    }

    std::uint32_t next_inclusive(std::uint32_t low, std::uint32_t high) noexcept
    {
        return low + (next() % (high - low + 1));
    }

private:
    std::uint32_t state_;
};

// Combines breath / saccade / blink. Each animator schedules its next firing
// in absolute milliseconds; tick() applies whichever are due.
class FaceAnimator {
public:
    FaceAnimator() = default;

    void tick(std::uint32_t now_ms, DrawContext& ctx);

private:
    void breath_tick(std::uint32_t now_ms, DrawContext& ctx);
    void saccade_tick(std::uint32_t now_ms, DrawContext& ctx);
    void blink_tick(std::uint32_t now_ms, DrawContext& ctx);

    std::uint32_t breath_next_ms_{0};
    std::uint32_t saccade_next_ms_{0};
    std::uint32_t blink_next_ms_{0};
    std::uint32_t breath_phase_{0};
    bool eyes_open_{true};
};

} // namespace stackchan::avatar::internal
