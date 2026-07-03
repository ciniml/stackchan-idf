// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "board/board.hpp"

namespace stackchan::app {

// Phase 1+2 QR scanner: spawns a worker that powers the GC0308 camera, grabs
// QVGA grayscale frames, runs quirc, and ESP_LOGI("qr", ...)s the decoded
// payload. Phase 3 will replace ESP_LOGI with a device_ui callback, Phase 4
// will add a BLE notify path — neither lives here yet, but the API stays
// small (start / stop) so those drop-ins don't have to change call sites.
//
// All entry points are safe to call before `board.begin()` succeeds in the
// sense that `start_qr_scan` returns false when the board kind doesn't
// support a camera (Takao base / Atom-nyan) — the task is simply never
// spawned. Camera support compiled out (CONFIG_STACKCHAN_CAMERA_ENABLED=n)
// also makes start_qr_scan return false at runtime.

// Returns true when a worker task was actually created. The task runs on
// CPU 0 at priority tskIDLE_PRIORITY+2, with an internal-RAM stack (quirc's
// flood-fill recursion + esp_camera DMA descriptors disallow PSRAM stack).
bool start_qr_scan(board::Board& board);

// Signal the worker to drain its in-flight frame, deinit the camera, cut
// power via AW9523, and self-delete. Safe to call when no scan is active.
void stop_qr_scan();

// True while the QR worker task is alive. The camera driver has a single
// framebuffer (fb_count=1), so anything else that wants the camera — e.g.
// the settings page's one-shot capture — must check this and back off
// instead of double-initialising esp_camera.
bool qr_scan_active();

} // namespace stackchan::app
