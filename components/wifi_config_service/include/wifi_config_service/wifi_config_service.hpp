// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

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

} // namespace stackchan::wifi_config
