// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "avatar/canvas.hpp"
#include "shared_state.hpp"

// Minimal status overlay for the AtomS3R (128x128) — the CoreS3 device_ui's
// 5-tab UI doesn't fit on a 128x128 panel and the AtomS3R has no LCD touch
// anyway, so this is a single full-screen status read-out toggled by the
// USER_BUT (M5.BtnA). Shows FW version, SSID, IP, Wi-Fi state, BLE state,
// and conversation status — enough to bring the device up without a phone /
// browser. Settings still happen over BLE / Wi-Fi.
namespace stackchan::app::atom_status {

void init(SharedState& state);

// Drive the toggle from the same task that calls M5.update() (demo_loop):
// short press of M5.BtnA flips active() between true and false.
void poll_button();

bool active();

// Render into the caller-owned drawing surface (render task's Canvas).
// Returns true if it repainted this frame; the caller presents (end_frame).
bool draw(avatar::RichCanvas& canvas);

} // namespace stackchan::app::atom_status
