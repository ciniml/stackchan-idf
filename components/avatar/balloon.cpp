// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "balloon.hpp"

#include <algorithm>

namespace stackchan::avatar::internal {

namespace {

// Geometry of the bottom-of-screen balloon. Panel dimensions are computed at
// draw time from the live canvas size so the same code paths drive both the
// CoreS3 (320x240) and the AtomS3R (128x128) displays.
constexpr std::int16_t kMargin = 4;
constexpr std::int16_t kPanelRadius = 10;
constexpr std::int16_t kInnerPadding = 8;
// Small panels (e.g. AtomS3R's 128x128) get the 12-px glyph; the 24-px glyph
// would eat half the screen height and leave no room for the avatar above.
constexpr std::int16_t kSmallPanelHeightThreshold = 160;
constexpr std::int16_t kBigPanelH = 40;   // for 24-px font
constexpr std::int16_t kSmallPanelH = 22; // for 12-px font

// Marquee tuning.
constexpr std::int32_t kScrollSpeedPxPerSec = 60;
// Gap (px) of "empty space" between the trailing edge of one pass and the
// leading edge of the next so the user perceives the message restarting.
constexpr std::int32_t kRepeatGapPx = 60;

// Default minimum display time for short (non-scrolling) text. The application
// can override with `Avatar::set_balloon_text(text, hold_ms)`.
constexpr std::uint32_t kDefaultStaticHoldMs = 3000;

} // namespace

void draw_balloon(RichCanvas& canvas, DrawContext& ctx)
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

    const std::int16_t canvas_w = static_cast<std::int16_t>(canvas.width());
    const std::int16_t canvas_h = static_cast<std::int16_t>(canvas.height());
    const bool small_panel = canvas_h <= kSmallPanelHeightThreshold;
    const auto* font = small_panel ? &fonts::lgfxJapanGothic_12 : &fonts::lgfxJapanGothic_24;
    const std::int16_t panel_h = small_panel ? kSmallPanelH : kBigPanelH;
    const std::int16_t panel_x = kMargin;
    const std::int16_t panel_w = canvas_w - kMargin * 2;
    const std::int16_t panel_y = canvas_h - panel_h - kMargin;

    // Composite the bottom balloon strip as one group (direct strategy clears +
    // blits the whole panel; buffered strategy treats it as a no-op).
    canvas.begin_group(panel_x, panel_y, panel_w, panel_h);
    canvas.fillRoundRect(panel_x, panel_y, panel_w, panel_h, kPanelRadius, bg);
    canvas.drawRoundRect(panel_x, panel_y, panel_w, panel_h, kPanelRadius, fg);

    canvas.setTextColor(fg, bg);
    canvas.setFont(font);
    canvas.setTextSize(1);

    const std::int32_t inner_x = panel_x + kInnerPadding;
    const std::int32_t inner_w = panel_w - 2 * kInnerPadding;
    const std::int32_t text_w = canvas.textWidth(text.c_str());
    const std::int32_t mid_y = panel_y + panel_h / 2;
    const std::uint32_t elapsed_ms = ctx.now_ms - ctx.balloon_set_ms;

    if (text_w <= inner_w) {
        // Text fits — static centered. Mark done after the configured hold.
        canvas.setTextDatum(lgfx::textdatum_t::middle_center);
        canvas.drawString(text.c_str(), panel_x + panel_w / 2, mid_y);

        const std::uint32_t hold_ms =
            std::max(ctx.balloon_hold_ms, kDefaultStaticHoldMs);
        if (elapsed_ms >= hold_ms) {
            ctx.balloon_done = true;
        }
        canvas.end_group();
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

    canvas.setClipRect(inner_x, panel_y, inner_w, panel_h);
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
    canvas.end_group();
}

} // namespace stackchan::avatar::internal
