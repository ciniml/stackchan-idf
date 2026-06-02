// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar/avatar.hpp"

#include <M5GFX.h>

#include "animation.hpp"
#include "balloon.hpp"
#include "effect.hpp"
#include "face.hpp"

namespace stackchan::avatar {

// Avatar is a pure renderer: it owns no display or framebuffer. The caller (the
// render task in main) owns the canvas and pushes it to the panel; tick() only
// composes the frame into the borrowed canvas. The face layout is rebuilt the
// first time tick() is called and whenever the canvas changes size — which
// lets the same Avatar drive both the CoreS3 (320x240) and AtomS3R (128x128)
// LCDs without per-board ifdefs.
class Avatar::Impl {
public:
    Impl() noexcept = default;

    void tick(std::uint32_t now_ms, RichCanvas& canvas)
    {
        animator_.tick(now_ms, context_);
        context_.now_ms = now_ms;

        // Lazy / size-tracked face rebuild — first tick or canvas dim change
        // re-runs build_face() against the live tuning so the geometry is
        // always in sync with the canvas size we're about to draw into.
        const std::int16_t cw = static_cast<std::int16_t>(canvas.width());
        const std::int16_t ch = static_cast<std::int16_t>(canvas.height());
        if (cw != last_canvas_w_ || ch != last_canvas_h_) {
            face_ = internal::build_face(tuning_, cw, ch);
            last_canvas_w_ = cw;
            last_canvas_h_ = ch;
            full_repaint_pending_ = true;
        }

        // A pending full repaint (expression / layout / palette change, or a
        // return from the on-device UI) is forwarded to the canvas so the
        // direct strategy clears the whole panel this frame; the buffered
        // strategy clears every frame regardless.
        if (full_repaint_pending_) {
            canvas.request_full_repaint();
            full_repaint_pending_ = false;
        }
        canvas.begin_frame(context_.palette.background);
        internal::draw_face(canvas, face_, context_);
        internal::draw_effect(canvas, context_);
        internal::draw_balloon(canvas, context_);
        // end_frame() (present) is the caller's responsibility, after it has
        // composited any overlays (e.g. the battery gauge) onto the same frame.
    }

    DrawContext& context() noexcept { return context_; }
    void request_full_repaint() noexcept { full_repaint_pending_ = true; }

    void set_face_tuning(const FaceTuning& tuning)
    {
        tuning_ = tuning;
        context_.palette.primary = tuning.face_color;
        context_.palette.background = tuning.bg_color;
        // Force a rebuild on the next tick (canvas size still valid).
        last_canvas_w_ = 0;
        last_canvas_h_ = 0;
        full_repaint_pending_ = true;
    }

private:
    DrawContext context_{};
    FaceTuning tuning_{};
    internal::Face face_{};
    internal::FaceAnimator animator_{};
    std::int16_t last_canvas_w_ = 0;
    std::int16_t last_canvas_h_ = 0;
    bool full_repaint_pending_ = true;
};

Avatar::Avatar() : impl_{std::make_unique<Impl>()} {}
Avatar::~Avatar() = default;
Avatar::Avatar(Avatar&&) noexcept = default;
Avatar& Avatar::operator=(Avatar&&) noexcept = default;

void Avatar::set_expression(Expression expression) noexcept
{
    impl_->context().expression = expression;
    impl_->request_full_repaint(); // effect appears/disappears, eye masks change
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
    impl_->request_full_repaint(); // background colour may have changed
}

void Avatar::request_full_repaint() noexcept
{
    impl_->request_full_repaint();
}

void Avatar::set_face_tuning(const FaceTuning& tuning) noexcept
{
    impl_->set_face_tuning(tuning);
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
    // The balloon strip won't be redrawn once it's gone, so the direct strategy
    // needs a full repaint to erase it from the persistent panel.
    impl_->request_full_repaint();
    ctx.balloon_hold_ms = 0;
    ctx.balloon_done = false;
}

bool Avatar::is_balloon_done() const noexcept
{
    return impl_->context().balloon_done;
}

void Avatar::tick(std::uint32_t now_ms, RichCanvas& canvas)
{
    impl_->tick(now_ms, canvas);
}

} // namespace stackchan::avatar
