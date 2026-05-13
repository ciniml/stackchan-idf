// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5GFX.h>

namespace stackchan::app {

// Start the Wi-Fi station — either by reconnecting to credentials stored in
// NVS or by running BLE-based ESP Unified Provisioning interactively. When
// the device is unprovisioned the QR code + service info needed by the
// "ESP BLE Provisioning" mobile app are rendered onto `display`.
//
// Blocks only until provisioning finishes (or returns immediately if NVS
// already has credentials). The Wi-Fi connection itself is asynchronous —
// poll wifi_is_connected() to find out when it has an IP.
//
// Returns true if the Wi-Fi driver was started (regardless of whether the
// AP is reachable yet).
bool wifi_connect_or_provision(M5GFX& display);

// True once Wi-Fi has an IP. Becomes false again on disconnect.
bool wifi_is_connected();

} // namespace stackchan::app
