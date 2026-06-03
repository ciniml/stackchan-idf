// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <wifi_config_service/wifi_config_service.hpp>

#include <esp_http_server.h>

namespace stackchan::wifi_config::http {

// Register all /api/* URI handlers and the static GET / root handler with
// the running HTTP server. The handlers share a process-wide singleton
// crypto::Session and staging buffer, mirroring the BLE service.
void register_handlers(httpd_handle_t server, const config::DeviceConfig& current);

// Update the Wi-Fi-connected flag exposed by /api/status.
void set_wifi_connected(bool connected);

// Update the cached battery snapshot (mV / mA / percent) exposed by /api/status.
void set_battery(int millivolts, int milliamps, int percent);

// Register the servo range-mode sink + live position getter. See
// wifi_config_service.hpp.
void set_servo_range_mode_sink(config::ServoRangeModeSink sink);
void set_servo_positions_getter(config::ServoPositionsGetter getter);

// Record the booted board kind for /api/status. See wifi_config_service.hpp.
void set_board_kind(std::uint8_t kind);

// Set the avatar bytecode sink (called by POST /api/avatar-dsl after the
// payload has been validated and persisted). See wifi_config_service.hpp.
void set_avatar_bytecode_sink(AvatarBytecodeSink sink);

} // namespace stackchan::wifi_config::http
