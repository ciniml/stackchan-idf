// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5GFX.h>

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
//   - draw(): called from the render task each frame; repaints (to an
//     off-screen sprite, pushed in one shot — no flicker) only when the page
//     or live status actually changed.
namespace stackchan::app::ui {

void init(SharedState& state);
void handle_tap(int x, int y);
bool active();
void draw(M5GFX& display);

} // namespace stackchan::app::ui
