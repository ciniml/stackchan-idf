// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "avatar/canvas.hpp"
#include "shared_state.hpp"

// On-device touchscreen UI: a tabbed info / settings / control screen reached
// by tapping the LCD's top-right corner. Owns its own layout + touch
// hit-testing so the layout lives in one place.
//
//   - handle_tap(): called from the task that runs M5.update() (demo_loop),
//     once per press. Maps the tap to a UI action (open/close, switch tab,
//     toggle, button) based on the current page.
//   - active(): true while the UI is shown (the render task draws the UI
//     instead of the avatar).
//   - draw(): called from the render task each frame; renders into the caller's
//     shared canvas only when the page or live status actually changed, and
//     returns whether it drew (so the caller pushes only then — no flicker).
namespace stackchan::app::ui {

void init(SharedState& state);
void handle_tap(int x, int y);
// Open / close the UI without going through a tap. Used by physical-button
// inputs (e.g. StopWatch's BtnA) when the corner of a round panel makes the
// tap-to-open hot zone awkward to hit. Mirrors the open/close behaviour of
// a tap in the avatar's top-right corner: load staged values + show 情報
// when opening, drop range mode + close when closing.
void toggle();
bool active();
// Render into the caller-owned drawing surface (the render task's Canvas).
// Returns true if it repainted this frame; the caller presents (end_frame) then.
bool draw(avatar::RichCanvas& canvas);

} // namespace stackchan::app::ui
