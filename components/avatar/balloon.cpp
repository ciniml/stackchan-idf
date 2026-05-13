// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "balloon.hpp"

#include <algorithm>

namespace stackchan::avatar::internal {

namespace {

// Geometry of the bottom-of-screen balloon (canvas is 320x240).
constexpr std::int16_t kMargin = 4;
constexpr std::int16_t kPanelH = 40;
constexpr std::int16_t kPanelX = kMargin;
constexpr std::int16_t kPanelY = 240 - kPanelH - kMargin;
constexpr std::int16_t kPanelW = 320 - kMargin * 2;
constexpr std::int16_t kPanelRadius = 10;
constexpr std::int16_t kInnerPadding = 8;
// Glyph height is ~24 px, which matches the lgfxJapanGothic_24 font used below.
const auto* const kBalloonFont = &fonts::lgfxJapanGothic_24;

// Marquee tuning.
constexpr std::int32_t kScrollSpeedPxPerSec = 60;
// Gap (px) of "empty space" between the trailing edge of one pass and the
// leading edge of the next so the user perceives the message restarting.
constexpr std::int32_t kRepeatGapPx = 60;

// Default minimum display time for short (non-scrolling) text. The application
// can override with `Avatar::set_balloon_text(text, hold_ms)`.
constexpr std::uint32_t kDefaultStaticHoldMs = 3000;

} // namespace

void draw_balloon(M5Canvas& canvas, DrawContext& ctx)
{
    if (!ctx.balloon_text.has_value()) {
        return;
    }
    const auto& text = *ctx.balloon_text;
    if (text.empty()) {
        return;
    }

    const std::uint16_t fg = ctx.palette.balloon_foreground;
    const std::uint16_t bg = ctx.palette.balloon_background;

    canvas.fillRoundRect(kPanelX, kPanelY, kPanelW, kPanelH, kPanelRadius, bg);
    canvas.drawRoundRect(kPanelX, kPanelY, kPanelW, kPanelH, kPanelRadius, fg);

    canvas.setTextColor(fg, bg);
    canvas.setFont(kBalloonFont);
    canvas.setTextSize(1);

    constexpr std::int32_t inner_x = kPanelX + kInnerPadding;
    constexpr std::int32_t inner_w = kPanelW - 2 * kInnerPadding;
    const std::int32_t text_w = canvas.textWidth(text.c_str());
    const std::int32_t mid_y = kPanelY + kPanelH / 2;
    const std::uint32_t elapsed_ms = ctx.now_ms - ctx.balloon_set_ms;

    if (text_w <= inner_w) {
        // Text fits — static centered. Mark done after the configured hold.
        canvas.setTextDatum(lgfx::textdatum_t::middle_center);
        canvas.drawString(text.c_str(), kPanelX + kPanelW / 2, mid_y);

        const std::uint32_t hold_ms =
            std::max(ctx.balloon_hold_ms, kDefaultStaticHoldMs);
        if (elapsed_ms >= hold_ms) {
            ctx.balloon_done = true;
        }
        return;
    }

    // Marquee: text starts just past the right inner edge and scrolls left.
    // A single "pass" travels `text_w + inner_w` pixels (entry + traverse +
    // exit). One full cycle adds `kRepeatGapPx` so the message restarts with
    // a perceivable gap.
    const std::int32_t one_pass_px = text_w + inner_w;
    const std::int32_t cycle_px = one_pass_px + kRepeatGapPx;
    const std::int32_t offset_in_cycle =
        static_cast<std::int32_t>(elapsed_ms) * kScrollSpeedPxPerSec / 1000 % cycle_px;
    const std::int32_t x = inner_x + inner_w - offset_in_cycle;

    canvas.setClipRect(inner_x, kPanelY, inner_w, kPanelH);
    canvas.setTextDatum(lgfx::textdatum_t::middle_left);
    canvas.drawString(text.c_str(), x, mid_y);
    canvas.clearClipRect();

    // Mark done once the message has scrolled across at least once
    // (or the caller-requested hold time has elapsed, whichever is longer).
    const std::uint32_t one_pass_ms =
        static_cast<std::uint32_t>(one_pass_px) * 1000u /
        static_cast<std::uint32_t>(kScrollSpeedPxPerSec);
    const std::uint32_t complete_at = std::max(ctx.balloon_hold_ms, one_pass_ms);
    if (elapsed_ms >= complete_at) {
        ctx.balloon_done = true;
    }
}

} // namespace stackchan::avatar::internal
