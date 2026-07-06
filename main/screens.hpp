// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5Unified.h>

#include "avatar/canvas.hpp"
#include "board/board.hpp"
#include "shared_state.hpp"

// The full-screen overlay stack: everything that can take the panel over
// from the avatar (SoftAP QR screen, the touch device_ui, the AtomS3R
// button-toggled status overlay), behind one interface so the render task
// and demo_loop dispatch to "the screens" instead of hard-coding each
// module's precedence and input quirks.
//
// Priority = registration order (fixed at init): ap_screen pre-empts the
// per-board settings UI, which pre-empts the avatar. Exactly one settings
// UI flavour is registered per boot (device_ui on touch boards, atom_status
// on button-only boards — BoardProfile::button_overlay_ui decides).
namespace stackchan::app::screens {

// One full-screen overlay. Implementations are thin adapters over the
// existing ap_screen / device_ui / atom_status modules — the interface is
// the seam, the modules keep their internals.
class Screen {
public:
    virtual ~Screen() = default;

    // Whether this screen currently owns the panel. The topmost active
    // screen gets draw(); everything below it (including the avatar) is
    // suppressed.
    virtual bool active() const = 0;

    // Render one tick. Canvas-based screens compose into `canvas` and
    // return true when they repainted (the caller presents via end_frame);
    // direct-to-panel screens (ap_screen needs M5GFX::qrcode, which the
    // Canvas abstraction doesn't expose) paint the display themselves and
    // return false so the stale canvas is never pushed over them.
    virtual bool draw(avatar::RichCanvas& canvas) = 0;

    // Input hooks. handle_tap may consume a tap even while inactive (the
    // device_ui owns its open-UI / mute hot corners on the avatar screen);
    // returning true stops propagation. poll_input runs every demo_loop
    // tick for button-driven screens (atom_status's BtnA gesture vocab).
    virtual bool handle_tap(int x, int y) { return false; }
    virtual void handle_flick(int dx, int dy) {}
    virtual void poll_input() {}
};

// Build the per-board stack (ap_screen + one settings-UI flavour) and init
// the underlying modules. Call once at boot, before the render task starts.
void init(M5GFX& display, SharedState& state, const stackchan::board::BoardProfile& profile);

// True when any overlay owns the panel (render task: suppress the avatar).
bool overlay_active();

// Draw the topmost active overlay. Returns true when the canvas should be
// presented (see Screen::draw). Only call while overlay_active().
bool draw_overlay(avatar::RichCanvas& canvas);

// Input dispatch in priority order. handle_tap returns true when some
// screen consumed the tap.
void poll_inputs();
bool handle_tap(int x, int y);
void handle_flick(int dx, int dy);

} // namespace stackchan::app::screens
