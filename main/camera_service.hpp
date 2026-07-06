// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "board/board.hpp"
#include "shared_state.hpp"

// Resident GC0308 camera lifecycle + the HTTP capture/register sinks.
//
// This module is the single owner of the resident camera instance and of the
// SharedState::i2c_quiesce coordination flag: every runtime SCCB access
// (capture session, colour-bar toggle, RAW driver swap, register pokes) runs
// under a quiesce window that parks the In_I2C pollers (demo_loop / render /
// led tasks) first — SCCB shares GPIO12/11 with In_I2C.
//
// Both entry points are safe no-ops on boards without a camera (and on
// builds with CONFIG_STACKCHAN_CAMERA_ENABLED=n, where they compile to
// stubs).
namespace stackchan::app::camera_service {

// Boot-time resident init (M5Base only — other boards no-op). Initialised
// ONCE, never deinited:
//   - per-capture esp_camera_init needs a contiguous internal-RAM DMA
//     bounce block that no longer exists once the conversation task's TLS
//     session is up (largest sinks to ~8 KiB). Boot-time init lands while
//     largest ≈ 130 KiB.
//   - init before ANY other task runs also removes the SCCB-vs-In_I2C race
//     window entirely: the sensor's register upload happens in
//     single-threaded context. After init the sensor free-runs (AGC/AWB
//     on-chip) and SCCB is only touched again under quiesce.
// Cost: DMA bounce internal RAM + 600 KiB PSRAM fb (VGA RGB565) + the cam
// task, held for the process lifetime, and the sensor streams continuously
// (~6 MB/s bounce memcpy into PSRAM at VGA). The QR task still creates its
// own CameraGc0308 — double-init would fail — so STACKCHAN_QR_TEST_AT_BOOT
// must stay off until qr_task is taught to reuse this instance.
// Failure is non-fatal: the sinks just never register and /api/status keeps
// has_camera=false.
void init_resident(stackchan::board::Board& board);

// Register the HTTP capture + register-access sinks (wifi_config_service).
// No-op unless init_resident succeeded — /api/status's has_camera flag
// derives from this registration, which is how the web UI decides to show
// the 撮影 section.
void register_sinks(SharedState& state);

} // namespace stackchan::app::camera_service
