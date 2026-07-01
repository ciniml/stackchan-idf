// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <string>

#include <tl/expected.hpp>

namespace stackchan::wifi_config::release_ota {

// Result of start() — the actual download / write runs on a worker task and
// reports progress via the existing config::ota::status_json() so the UI
// polls /api/ota/status as it does for browser-uploaded OTA.
enum class StartError {
    AlreadyRunning,
    BadTag,            // tag missing / contains chars that don't belong in a URL
    UnknownBoard,      // g_board_kind not in {cores3, atoms3r, atoms3, stopwatch}
    WorkerSpawnFailed, // xTaskCreate failed (heap)
};

// Kick off a release-firmware download + OTA-write + reboot. tag = git tag
// like "v0.7.3"; board_kind = the byte values used by config_service
// (M5Base/TakaoBase=0/1 → "cores3", AtomNyan=2 → "atoms3r", AtomS3=3 →
// "atoms3", StopWatch=4 → "stopwatch"). Returns immediately; the caller
// must poll /api/ota/status for progress.
[[nodiscard]] tl::expected<void, StartError> start(const std::string& tag,
                                                    std::uint8_t board_kind);

// True while the worker task is alive (download in flight). Used by /api/ota
// /control "abort" to forward the cancel to the HTTP client cleanly.
bool active();

// Request the worker to stop at the next chunk boundary. Safe to call from
// any task. The worker aborts the in-progress OTA and self-deletes; status
// reflects "failed" with error "aborted".
void request_abort();

// Map the board_kind byte to the URL slug used in
// firmware-<tag>-<slug>.zip / firmware/<tag>/<slug>/stackchan_idf.bin.
// Returns nullptr for unknown values.
const char* board_slug(std::uint8_t board_kind);

// Fetch the Pages-hosted versions.json (list of published tags + per-board
// asset filenames) via HTTPS. Writes the raw response body into `out` on
// success. Returns false on any failure (STA down, HTTPS handshake,
// non-200, oversized body). Blocks the caller for up to ~15 s; spawns a
// worker task internally so the handler stack (6 KiB) doesn't have to
// hold the mbedTLS handshake state.
bool fetch_versions_json(std::string& out);

} // namespace stackchan::wifi_config::release_ota
