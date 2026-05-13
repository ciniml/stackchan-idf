// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar/avatar.hpp"

#include <M5GFX.h>
#include <esp_log.h>

#include "animation.hpp"
#include "balloon.hpp"
#include "effect.hpp"
#include "face.hpp"

namespace stackchan::avatar {

namespace {
constexpr const char* kTag = "avatar";
constexpr std::int16_t kCanvasWidth = 320;
constexpr std::int16_t kCanvasHeight = 240;
} // namespace

class Avatar::Impl {
public:
    // CoreS3 LCD shares MISO with the panel's DC line on GPIO35, which makes
    // any SPI read from the display hang. Constructing the canvas with the
    // display as parent triggers a readRect on createSprite, so build the
    // canvas as a standalone sprite and push it explicitly with a target.
    explicit Impl(M5GFX& display) noexcept : display_{display}, canvas_{} {}

    bool begin()
    {
        canvas_.setColorDepth(16);
        canvas_.setPsram(true);
        if (canvas_.createSprite(kCanvasWidth, kCanvasHeight) == nullptr) {
            ESP_LOGE(kTag, "createSprite(%d, %d) failed (need PSRAM)", kCanvasWidth, kCanvasHeight);
            return false;
        }
        return true;
    }

    void tick(std::uint32_t now_ms)
    {
        animator_.tick(now_ms, context_);
        context_.now_ms = now_ms;

        canvas_.fillScreen(context_.palette.background);
        internal::draw_face(canvas_, face_, context_);
        internal::draw_effect(canvas_, context_);
        internal::draw_balloon(canvas_, context_);
        canvas_.pushSprite(&display_, 0, 0);
    }

    DrawContext& context() noexcept { return context_; }

private:
    M5GFX& display_;
    M5Canvas canvas_;
    DrawContext context_{};
    internal::Face face_{};
    internal::FaceAnimator animator_{};
};

Avatar::Avatar(M5GFX& display) : impl_{std::make_unique<Impl>(display)} {}
Avatar::~Avatar() = default;
Avatar::Avatar(Avatar&&) noexcept = default;
Avatar& Avatar::operator=(Avatar&&) noexcept = default;

bool Avatar::begin()
{
    return impl_->begin();
}

void Avatar::set_expression(Expression expression) noexcept
{
    impl_->context().expression = expression;
}

void Avatar::set_mouth_open(float ratio) noexcept
{
    if (ratio < 0.0f) {
        ratio = 0.0f;
    } else if (ratio > 1.0f) {
        ratio = 1.0f;
    }
    impl_->context().mouth_open_ratio = ratio;
}

void Avatar::set_gaze(float horizontal, float vertical) noexcept
{
    impl_->context().gaze_horizontal = horizontal;
    impl_->context().gaze_vertical = vertical;
}

void Avatar::set_palette(const Palette& palette) noexcept
{
    impl_->context().palette = palette;
}

void Avatar::set_balloon_text(std::string_view text, std::uint32_t hold_ms)
{
    auto& ctx = impl_->context();
    ctx.balloon_text = std::string{text};
    ctx.balloon_hold_ms = hold_ms;
    ctx.balloon_done = false;
    // Resync marquee phase to "now" so a fresh string always enters from the
    // right edge. context_.now_ms was last updated by tick().
    ctx.balloon_set_ms = ctx.now_ms;
}

void Avatar::clear_balloon() noexcept
{
    auto& ctx = impl_->context();
    ctx.balloon_text.reset();
    ctx.balloon_hold_ms = 0;
    ctx.balloon_done = false;
}

bool Avatar::is_balloon_done() const noexcept
{
    return impl_->context().balloon_done;
}

void Avatar::tick(std::uint32_t now_ms)
{
    impl_->tick(now_ms);
}

} // namespace stackchan::avatar
