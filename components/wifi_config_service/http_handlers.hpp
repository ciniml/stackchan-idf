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

// Mark the service as serving the on-device SoftAP — bypasses require_auth
// while set, and surfaces "provisioning_mode" in /api/status.
void set_provisioning_mode(bool active);

// Update the cached battery snapshot (mV / mA / percent) exposed by /api/status.
void set_battery(int millivolts, int milliamps, int percent);

// Register the servo range-mode sink + live position getter. See
// wifi_config_service.hpp.
void set_servo_range_mode_sink(config::ServoRangeModeSink sink);
void set_servo_positions_getter(config::ServoPositionsGetter getter);

// Register the audio pipeline metrics JSON getter. The same callable is also
// wired into BLE chr 0x1f via config_service; HTTP just calls it on
// `GET /api/metrics/audio`. nullptr leaves the endpoint returning "{}".
void set_audio_metrics_getter(config::AudioMetricsJsonGetter getter);

// LED live-state read/write hooks. The closures are shared with BLE chr 0x20.
// Read-out drives `GET /api/led-state`; the patch sink fires on
// `POST /api/led-state` once the body has been parsed.
void set_led_state_getter(config::LedStateGetter getter);
void set_led_state_sink(config::LedStateSink sink);

// Mic lip-sync gain getter/sink — see wifi_config_service.hpp.
void set_mic_lip_gain_getter(config::MicLipGainGetter getter);
void set_mic_lip_gain_sink(config::MicLipGainSink sink);

// Speaker volume getter/sink — see wifi_config_service.hpp.
void set_speaker_volume_getter(config::SpeakerVolumeGetter getter);
void set_speaker_volume_sink(config::SpeakerVolumeSink sink);

// JTTS test-say sink — see wifi_config_service.hpp.
void set_jtts_say_kana_sink(config::JttsSayKanaSink sink);

// Record the booted board kind for /api/status. See wifi_config_service.hpp.
void set_board_kind(std::uint8_t kind);

// Set the avatar bytecode sink (called by POST /api/avatar-dsl after the
// payload has been validated and persisted). See wifi_config_service.hpp.
void set_avatar_bytecode_sink(AvatarBytecodeSink sink);

// 音声 DB (/api/voice-db) の sink / status getter。See wifi_config_service.hpp.
void set_voice_db_sink(VoiceDbSink sink);
void set_voice_db_status_getter(VoiceDbStatusGetter getter);
void set_hmm_voice_sink(HmmVoiceSink sink);
void set_hmm_voice_status_getter(HmmVoiceStatusGetter getter);

// Register the one-shot camera capture sink (GET /api/camera/capture).
// See wifi_config_service.hpp for the contract.
void set_camera_capture_sink(CameraCaptureSink sink);

// Register the sensor register-access sink (GET/POST /api/camera/reg).
// See wifi_config_service.hpp for the contract.
void set_camera_reg_sink(CameraRegSink sink);

// /mcp/* endpoint sinks. See wifi_config_service.hpp for the contract.
void set_mcp_say_kana_sink(McpSayKanaSink sink);
void set_lt_config_sink(LtConfigSink sink);
void set_mcp_expression_sink(McpExpressionSink sink);
void set_mcp_balloon_sink(McpBalloonSink sink);

} // namespace stackchan::wifi_config::http
