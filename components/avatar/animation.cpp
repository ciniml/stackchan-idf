#include "animation.hpp"

#include <cmath>
#include <numbers>

namespace stackchan::avatar::internal {

void FaceAnimator::breath_tick(std::uint32_t now_ms, DrawContext& ctx)
{
    breath_phase_ = (breath_phase_ + 1) % 100;
    const float f = std::sin(static_cast<float>(breath_phase_) * 2.0f * std::numbers::pi_v<float> / 100.0f);
    ctx.breath = f;
    breath_next_ms_ = now_ms + 33;
}

void FaceAnimator::saccade_tick(std::uint32_t now_ms, DrawContext& ctx)
{
    XorShift32 rng{ctx.rng_state};
    ctx.gaze_horizontal = rng.next_range(-1.0f, 1.0f);
    ctx.gaze_vertical = rng.next_range(-1.0f, 1.0f);
    const std::uint32_t delay = 500 + 100 * rng.next_inclusive(0, 20);
    ctx.rng_state = rng.next();
    saccade_next_ms_ = now_ms + delay;
}

void FaceAnimator::blink_tick(std::uint32_t now_ms, DrawContext& ctx)
{
    XorShift32 rng{ctx.rng_state};
    eyes_open_ = !eyes_open_;
    std::uint32_t delay;
    if (eyes_open_) {
        ctx.eye_open_ratio = 1.0f;
        delay = 2500 + 100 * rng.next_inclusive(0, 20);
    } else {
        ctx.eye_open_ratio = 0.0f;
        delay = 300 + 10 * rng.next_inclusive(0, 20);
    }
    ctx.rng_state = rng.next();
    blink_next_ms_ = now_ms + delay;
}

void FaceAnimator::tick(std::uint32_t now_ms, DrawContext& ctx)
{
    if (now_ms >= breath_next_ms_) {
        breath_tick(now_ms, ctx);
    }
    if (now_ms >= saccade_next_ms_) {
        saccade_tick(now_ms, ctx);
    }
    if (now_ms >= blink_next_ms_) {
        blink_tick(now_ms, ctx);
    }
}

} // namespace stackchan::avatar::internal
