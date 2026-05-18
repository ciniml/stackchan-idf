// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <config_service/config_service.hpp>

#include <esp_http_server.h>

namespace stackchan::wifi_config::http {

// Register all /api/* URI handlers and the static GET / root handler with
// the running HTTP server. The handlers share a process-wide singleton
// crypto::Session and staging buffer, mirroring the BLE service.
void register_handlers(httpd_handle_t server, const config::DeviceConfig& current);

// Update the Wi-Fi-connected flag exposed by /api/status.
void set_wifi_connected(bool connected);

} // namespace stackchan::wifi_config::http
