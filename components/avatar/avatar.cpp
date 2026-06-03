// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar/avatar.hpp"

#include <esp_log.h>

#include "animation.hpp"
#include "avatar_vm/bytecode.hpp"
#include "avatar_vm/default_bytecode.hpp"
#include "avatar_vm/vm.hpp"
#include "balloon.hpp"

namespace stackchan::avatar {

namespace {
constexpr const char* TAG = "avatar";
} // namespace

// Avatar is a pure renderer: it owns no display or framebuffer. The caller (the
// render task in main) owns the canvas and pushes it to the panel; tick() only
// composes the frame into the borrowed canvas.
//
// The face is drawn by an avatar_vm bytecode interpreter. The bytecode is
// either the firmware-embedded default (assets/default_face.avdsl compiled at
// build time) or a user-supplied override loaded into `loaded_bytecode_`. The
// VM is canvas-size agnostic — the same bytecode drives CoreS3 (320x240) and
// AtomS3R (128x128).
class Avatar::Impl {
public:
    Impl()
    {
        load_default();
    }

    void tick(std::uint32_t now_ms, RichCanvas& canvas)
    {
        animator_.tick(now_ms, context_);
        context_.now_ms = now_ms;

        if (full_repaint_pending_) {
            canvas.request_full_repaint();
            full_repaint_pending_ = false;
        }
        canvas.begin_frame(context_.palette.background);

        if (bytecode_ok_) {
            auto r = vm_.run(bytecode_, canvas, context_, tuning_);
            if (!r) {
                ESP_LOGW(TAG, "vm.run failed: %s", avatar_vm::to_string(r.error()));
                // One-shot warning per failure; keep trying so a transient OOM
                // doesn't permanently brick rendering.
            }
        }
        internal::draw_balloon(canvas, context_);
        // end_frame() (present) is the caller's responsibility, after it has
        // composited any overlays (e.g. the battery gauge) onto the same frame.
    }

    DrawContext& context() noexcept { return context_; }
    FaceTuning& tuning() noexcept { return tuning_; }
    void request_full_repaint() noexcept { full_repaint_pending_ = true; }

    void set_face_tuning(const FaceTuning& tuning)
    {
        tuning_ = tuning;
        context_.palette.primary = tuning.face_color;
        context_.palette.background = tuning.bg_color;
        full_repaint_pending_ = true;
    }

    bool load_bytecode(std::span<const std::uint8_t> bytes)
    {
        // Copy the buffer in so the VM owns its lifetime — callers might pass
        // a transient receive buffer (BLE chunk reassembly, HTTP body) that
        // would otherwise be freed before the next tick.
        loaded_bytecode_.assign(bytes.begin(), bytes.end());
        auto bc = avatar_vm::decode(loaded_bytecode_);
        if (!bc) {
            ESP_LOGE(TAG, "decode failed: %s", avatar_vm::to_string(bc.error()));
            load_default();
            return false;
        }
        bytecode_ = std::move(*bc);
        bytecode_ok_ = true;
        full_repaint_pending_ = true;
        return true;
    }

    void load_default()
    {
        auto bytes = avatar_vm::default_face_bytecode();
        loaded_bytecode_.assign(bytes.begin(), bytes.end());
        auto bc = avatar_vm::decode(loaded_bytecode_);
        if (!bc) {
            ESP_LOGE(TAG, "default decode failed: %s", avatar_vm::to_string(bc.error()));
            bytecode_ok_ = false;
            return;
        }
        bytecode_ = std::move(*bc);
        bytecode_ok_ = true;
        full_repaint_pending_ = true;
    }

private:
    DrawContext context_{};
    FaceTuning tuning_{};
    internal::FaceAnimator animator_{};
    std::vector<std::uint8_t> loaded_bytecode_;
    avatar_vm::Bytecode bytecode_{};
    avatar_vm::Vm vm_{};
    bool bytecode_ok_ = false;
    bool full_repaint_pending_ = true;
};

Avatar::Avatar() : impl_{std::make_unique<Impl>()} {}
Avatar::~Avatar() = default;
Avatar::Avatar(Avatar&&) noexcept = default;
Avatar& Avatar::operator=(Avatar&&) noexcept = default;

void Avatar::set_expression(Expression expression) noexcept
{
    impl_->context().expression = expression;
    impl_->request_full_repaint();
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
    impl_->request_full_repaint();
}

void Avatar::request_full_repaint() noexcept
{
    impl_->request_full_repaint();
}

void Avatar::set_face_tuning(const FaceTuning& tuning) noexcept
{
    impl_->set_face_tuning(tuning);
}

bool Avatar::load_face_bytecode(std::span<const std::uint8_t> bytes)
{
    return impl_->load_bytecode(bytes);
}

void Avatar::reset_face_bytecode() noexcept
{
    impl_->load_default();
}

void Avatar::set_balloon_text(std::string_view text, std::uint32_t hold_ms)
{
    auto& ctx = impl_->context();
    ctx.balloon_text = std::string{text};
    ctx.balloon_hold_ms = hold_ms;
    ctx.balloon_done = false;
    ctx.balloon_set_ms = ctx.now_ms;
}

void Avatar::clear_balloon() noexcept
{
    auto& ctx = impl_->context();
    ctx.balloon_text.reset();
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
