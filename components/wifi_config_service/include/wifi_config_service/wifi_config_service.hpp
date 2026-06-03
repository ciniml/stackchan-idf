// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <config_service/config_service.hpp>
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

// Update the battery snapshot (mV / mA / percent) surfaced by /api/status.
// No-op until the HTTP server has started. Thread-safe.
void set_battery(int millivolts, int milliamps, int percent);

// Register the servo range-setting mode sink and live-position getter. See
// config_service.hpp for the contract; the Wi-Fi service shares the same
// types. POST /api/servo-range-mode forwards to the sink; /api/status pulls
// from the getter.
void set_servo_range_mode_sink(config::ServoRangeModeSink sink);
void set_servo_positions_getter(config::ServoPositionsGetter getter);

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

} // namespace stackchan::wifi_config
