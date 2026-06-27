// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5Unified.h>

// Full-screen Wi-Fi join QR screen shown while SoftAP provisioning mode is
// active. Renders a QR encoding `WIFI:T:WPA;S:<SSID>;P:<PW>;H:false;;` (iOS
// Camera and modern Android decode this and offer one-tap join), plus the
// SSID / PW / URL in human-readable text below it. Takes over the panel
// outright — no avatar / battery / atom_status compositing — and yields the
// screen back to the avatar when wifi_ap_active() returns false.
namespace stackchan::app::ap_screen {

// True iff wifi_ap_active() is currently true. Render task uses this to gate.
bool active();

// Paint the QR + info to the panel. Idempotent within a refresh interval —
// returns true when it actually painted this frame.
bool draw(M5GFX& display);

// Touch handling. Returns true when AP screen owns the tap (= AP is up).
// Inside, taps on the on-screen "終了" button disable AP mode; taps that
// missed the button are still swallowed so they don't leak through to the
// device_ui underneath.
bool handle_tap(int x, int y);

} // namespace stackchan::app::ap_screen
