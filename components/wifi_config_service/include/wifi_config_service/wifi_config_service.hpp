// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <config_service/config_service.hpp>
#include <esp_http_server.h>
#include <tl/expected.hpp>

namespace stackchan::wifi_config {

enum class Error {
    AlreadyStarted,
    MdnsInit,
    HttpServerStart,
};

// Bring up mDNS (hostname `stackchan-XXXXXX.local`) and the HTTP settings
// server on port 80. Must be called after Wi-Fi STA has an IP address.
// Safe to call multiple times — subsequent calls are no-ops.
//
// The `current` config is used to seed the in-memory snapshot the HTTP
// handlers serve. Staging writes accumulate in a local buffer and only
// commit on /api/apply, identical to the BLE service.
tl::expected<void, Error> start(const config::DeviceConfig& current);

// Update Wi-Fi connectivity state — used by the /api/status endpoint to
// reflect the same flags the BLE Status characteristic does. Thread-safe.
void notify_wifi_connected(bool connected);

// Raw httpd handle. nullptr until the init task has started the server.
// Used by main's SoftAP captive portal to register a 404 catch-all on the
// same server (the captive portal must NOT spin its own httpd — only two
// open sockets, see start_http_server).
httpd_handle_t handle();

// Mark the HTTP service as serving the on-device SoftAP (provisioning)
// rather than the home Wi-Fi STA. While set:
//   - require_auth() is bypassed (physical AP button = implicit trust;
//     iOS doesn't reliably carry the Basic auth prompt past a captive
//     portal redirect anyway)
//   - GET /api/status surfaces `"provisioning_mode": true` so the web UI
//     can render an explanatory banner.
// Idempotent. Thread-safe.
void set_provisioning_mode(bool active);

// Update the battery snapshot (mV / mA / percent) surfaced by /api/status.
// No-op until the HTTP server has started. Thread-safe.
void set_battery(int millivolts, int milliamps, int percent);

// Register the servo range-setting mode sink and live-position getter. See
// config_service.hpp for the contract; the Wi-Fi service shares the same
// types. POST /api/servo-range-mode forwards to the sink; /api/status pulls
// from the getter.
void set_servo_range_mode_sink(config::ServoRangeModeSink sink);
void set_servo_positions_getter(config::ServoPositionsGetter getter);

// Register the audio pipeline metrics JSON getter (`GET /api/metrics/audio`).
// See config_service::set_audio_metrics_getter for the contract. nullptr
// leaves the HTTP endpoint returning "{}".
void set_audio_metrics_getter(config::AudioMetricsJsonGetter getter);

// LED live-state read/write hooks. `GET /api/led-state` returns the current
// values via the getter; `POST /api/led-state` parses JSON
// `{"mode":..,"r":..,"g":..,"b":..,"brightness":..}` (all optional) and
// forwards the patch to the sink.
void set_led_state_getter(config::LedStateGetter getter);
void set_led_state_sink(config::LedStateSink sink);

// Mic lip-sync gain getter/sink — `GET/POST /api/mic-lip-gain`. Same shape as
// LED above; same closures are also wired into BLE chr 0x23 via config_service.
void set_mic_lip_gain_getter(config::MicLipGainGetter getter);
void set_mic_lip_gain_sink(config::MicLipGainSink sink);

// Speaker volume getter/sink — `GET/POST /api/speaker-volume`. Same
// closures are also wired into BLE chr (config_service) so the live
// value is consistent across transports.
void set_speaker_volume_getter(config::SpeakerVolumeGetter getter);
void set_speaker_volume_sink(config::SpeakerVolumeSink sink);

// JTTS test-say sink — `POST /api/jtts-say`. Body is UTF-8 kana text;
// the same sink is also wired to the BLE jtts-say chr.
void set_jtts_say_kana_sink(config::JttsSayKanaSink sink);

// Record the booted board kind (mirrors board::BoardKind cast to byte) so it
// surfaces in /api/status under the "board" key. The web UI uses this to
// hide controls that don't apply to the current hardware. See
// config_service::set_board_kind for the byte values.
void set_board_kind(std::uint8_t kind);

// Sink called by `POST /api/avatar-dsl` after the bytecode has been
// validated and persisted to NVS. The app passes Avatar::load_face_bytecode
// here so an upload takes effect live, without rebooting.
// Returns true on success (bytecode applied), false on failure.
using AvatarBytecodeSink = std::function<bool(const std::uint8_t* data, std::size_t len)>;
void set_avatar_bytecode_sink(AvatarBytecodeSink sink);

// One-shot camera capture for `GET /api/camera/capture`. The sink fills
// `out` with a raw row-major frame, reports its dimensions, and names the
// pixel encoding in `format` — served verbatim as the X-Frame-Format
// response header. Current encodings:
//   "gray8"        — 1 B/px grayscale
//   "rgb565be"     — 2 B/px RGB565, big-endian (high byte first)
//   "bayer8-rggb"  — 1 B/px raw Bayer mosaic, RGGB phase (ISP bypassed)
// Returns false when the camera is unavailable (wrong board, QR scan
// holding the driver, low memory, sensor error). Registering a sink is
// also what makes /api/status report `"has_camera":true` — boards without
// a camera simply never register.
//
// Options (from the request's query string):
//   raw_bayer — ?fmt=raw: capture the raw Bayer mosaic instead of the
//               processed colour frame (colour-tuning diagnostics).
//   colorbar  — ?test=colorbar: sensor-generated test pattern instead of
//               the optical image (transfer-path validation).
// The sink runs on the HTTP server task (6 KiB internal-RAM stack) and may
// block for a couple of seconds (frame settle; the raw path re-inits the
// driver) — acceptable for a user-initiated photo button, but do not call
// anything heavier from it.
struct CameraCaptureOptions {
    bool raw_bayer = false;
    bool colorbar = false;
};
using CameraCaptureSink =
    std::function<bool(const CameraCaptureOptions& options, std::vector<std::uint8_t>& out,
                       std::size_t& width, std::size_t& height, std::string& format)>;
void set_camera_capture_sink(CameraCaptureSink sink);

// Raw sensor-register access for `GET/POST /api/camera/reg` — interactive
// colour tuning (AWB gains / colour matrix / gamma) against a test chart.
// `write` false: read register `reg` on `page` into `value`. `write` true:
// write `value`. Returns false on bus error / camera unavailable. Same
// HTTP-task constraints as the capture sink.
using CameraRegSink =
    std::function<bool(bool write, std::uint8_t page, std::uint8_t reg, std::uint8_t& value)>;
void set_camera_reg_sink(CameraRegSink sink);

// --- Channel API sinks (POST /mcp/* endpoints) -------------------------
//
// All sinks are called from the HTTP server task (6 KiB stack). The sinks
// must return quickly — long-running work (TTS synthesis, audio playback)
// belongs in a separate task spawned by the sink itself, NOT in the
// handler thread, because the next POST will block while we're synthesising.
//
// Empty `MCP_API_TOKEN` (Kconfig) disables the endpoints regardless of
// whether sinks are registered.

// `POST /mcp/say` — speak kana text. Implementation must enqueue or spawn
// — returning means "scheduled", not "spoken".
using McpSayKanaSink = std::function<void(std::string_view kana)>;
void set_mcp_say_kana_sink(McpSayKanaSink sink);

// LT timekeeper config JSON (live-apply, same contract as the BLE side's
// LtConfigSink). POST /api/lt-config delivers the raw JSON here.
using LtConfigSink = std::function<void(std::string_view json)>;
void set_lt_config_sink(LtConfigSink sink);

// `POST /mcp/expression` — set avatar face expression.
//   name ∈ {"neutral","happy","sad","angry","doubt","sleepy"}
using McpExpressionSink = std::function<void(std::string_view name)>;
void set_mcp_expression_sink(McpExpressionSink sink);

// `POST /mcp/balloon?hold_ms=N` — show text in the avatar balloon.
// `hold_ms == 0` means "use balloon defaults" (Avatar::set_balloon_text).
using McpBalloonSink = std::function<void(std::string_view text, std::uint32_t hold_ms)>;
void set_mcp_balloon_sink(McpBalloonSink sink);

} // namespace stackchan::wifi_config
