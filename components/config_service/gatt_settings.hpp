// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <config_service/config_service.hpp>

namespace stackchan::config::gatt {

// Register GATT service tables with NimBLE host and snapshot the active config.
// Must be called after nimble_port_init() but before nimble_port_freertos_init().
void init(const DeviceConfig& active);

// Attribute handle for the Status characteristic (used for notify).
uint16_t status_val_handle();

// Update CCCD subscription state. Called from GAP SUBSCRIBE event handler.
void set_subscribe(uint16_t conn_handle, bool subscribed);

// Update cached Wi-Fi state and send Status NOTIFY if subscribed.
// Thread-safe — may be called from any task.
void set_wifi_connected(bool connected);

// Drop application-layer crypto session state. Call on BLE disconnect so the
// next connection runs a fresh X25519 handshake.
void reset_session();

// Register the audio playback sink. See config_service.hpp for the contract.
void set_audio_stream_sink(const AudioStreamSink* sink);

// Register the live face-config callback. See config_service.hpp for the contract.
void set_face_config_sink(FaceConfigSink sink);

} // namespace stackchan::config::gatt
