// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "gatt_settings.hpp"
#include <config_service/config_store.hpp>
#include <config_service/crypto.hpp>
#include <config_service/ota.hpp>
#include <config_service/staged_config.hpp>
#include <avatar_vm/storage.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <mbedtls/sha256.h>

#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_uuid.h>
#include <host/ble_hs_mbuf.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

namespace stackchan::config::gatt {

namespace {

constexpr const char* kTag = "cfg-gatt";

// --- UUIDs — 128-bit, stored little-endian (byte[0] = LSB of the 128-bit value).
// Service: e3f0a000-7b1c-4d2a-9e6f-2c5a8d4b1f00
// SSID:    e3f0a001-...  Pass: e3f0a002-...  Key: e3f0a003-...
// Apply:   e3f0a004-...  Status: e3f0a005-...  KeyExchange: e3f0a006-...
// OpenAiEnabled: e3f0a007-...  JttsConfig: e3f0a008-...
// OtaControl: e3f0a009-...  OtaData: e3f0a00a-...
// Provider: e3f0a00b-...  GeminiApiKey: e3f0a00c-...  WifiIp: e3f0a00d-...
// AudioCtrl: e3f0a00e-...  AudioData: e3f0a00f-...  AudioCredit: e3f0a010-...
// RtpEnabled: e3f0a011-...  WifiMac: e3f0a012-...
// XiaoZhiUrl: e3f0a013-...  XiaoZhiToken: e3f0a014-...

static const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x00, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kSsidUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x01, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kPassUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x02, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kApiKeyUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x03, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kApplyUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x04, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kStatusUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x05, 0xa0, 0xf0, 0xe3);
// KeyExchange — the only plaintext characteristic, used to bootstrap the
// X25519 handshake. READ returns the device's 32-byte ephemeral pubkey;
// WRITE accepts the central's 32-byte pubkey and derives the AES-GCM key.
static const ble_uuid128_t kKeyExchangeUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x06, 0xa0, 0xf0, 0xe3);
// OpenAiEnabled — encrypted 1-byte flag (0=disabled, 1=enabled). The API key
// is kept independently in NVS; this is a master switch the user can flip to
// take Stack-chan offline without forgetting their setup.
static const ble_uuid128_t kOpenAiEnabledUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x07, 0xa0, 0xf0, 0xe3);
// RtpAudioEnabled — encrypted 1-byte flag (0=disabled, 1=enabled). Master
// switch for the Wi-Fi RTP live-audio receiver (main/wifi_audio.cpp). Takes
// effect on the next boot (Apply reboots), like OpenAiEnabled.
static const ble_uuid128_t kRtpAudioEnabledUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x11, 0xa0, 0xf0, 0xe3);
// JttsConfig — encrypted JSON document carrying babble voice parameters and
// the phrase list. Empty string falls back to compile-time defaults.
static const ble_uuid128_t kJttsConfigUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x08, 0xa0, 0xf0, 0xe3);
// OtaControl — encrypted JSON command channel for OTA flashing.
// READ returns the current status JSON, WRITE accepts begin/end/abort.
static const ble_uuid128_t kOtaControlUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x09, 0xa0, 0xf0, 0xe3);
// OtaData — encrypted WRITE-only firmware chunk channel. Each write is one
// 512-byte plaintext slice of the new image, applied sequentially.
static const ble_uuid128_t kOtaDataUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x0a, 0xa0, 0xf0, 0xe3);
// Provider — encrypted 1-byte enum (0=OpenAI, 1=Gemini). Selects which
// realtime conversation backend the conversation task talks to at boot.
static const ble_uuid128_t kProviderUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x0b, 0xa0, 0xf0, 0xe3);
// GeminiApiKey — encrypted string, paired with kKeyApiKey for OpenAI. Stored
// separately so users can configure both providers and flip between them.
static const ble_uuid128_t kGeminiApiKeyUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x0c, 0xa0, 0xf0, 0xe3);
// WifiIp — encrypted READ-only string with the current STA IPv4 address
// (e.g. "192.168.1.42"), or empty string when Wi-Fi is down. Lets the
// browser surface a fallback link for environments where the corresponding
// mDNS hostname (stackchan-XXXXXX.local) doesn't resolve — Android Chrome,
// Windows without Bonjour, locked-down corporate networks etc.
static const ble_uuid128_t kWifiIpUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x0d, 0xa0, 0xf0, 0xe3);
// WifiMac — encrypted READ-only string with the Wi-Fi STA MAC
// ("aa:bb:cc:dd:ee:ff"). The mDNS hostname is stackchan-<lower 3 bytes>, so the
// browser builds the .local URL from this directly instead of guessing it from
// the BLE name. Also surfaced in the UI as a device identifier.
static const ble_uuid128_t kWifiMacUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x12, 0xa0, 0xf0, 0xe3);
// AudioControl — encrypted JSON command channel for the BLE audio streamer.
// WRITE accepts {"op":"begin","codec":"aac","sample_rate":24000,"channels":1}
// / {"op":"end"} / {"op":"abort"}. READ is unused (returns empty).
static const ble_uuid128_t kAudioCtrlUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x0e, 0xa0, 0xf0, 0xe3);
// AudioData — plaintext WRITE-without-response AAC ADTS bytes. Each chunk is
// appended to the sink's stream buffer; the AAC decoder syncs on ADTS headers
// so chunking can be arbitrary (no need to align to frame boundaries).
static const ble_uuid128_t kAudioDataUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x0f, 0xa0, 0xf0, 0xe3);
// AudioCredit — plaintext READ-only uint32 LE: the number of bytes the audio
// sink can accept right now without dropping. Because AudioData is written
// without response there's no per-write backpressure, so the sender polls
// this for credit-based flow control: read credit, send up to that many
// bytes, read again. Free space only grows between reads (single producer,
// drains as it plays), so it's a safe lower bound — the sender never
// overflows the device. 0 means "full, wait"; also 0 with no active session.
static const ble_uuid128_t kAudioCreditUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x10, 0xa0, 0xf0, 0xe3);
// XiaoZhiUrl — encrypted string, the full WebSocket endpoint of a XiaoZhi AI
// server ("ws://<host>:8000/xiaozhi/v1/"). Used only when provider == XiaoZhi.
static const ble_uuid128_t kXiaozhiUrlUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x13, 0xa0, 0xf0, 0xe3);
// XiaoZhiToken — encrypted string bearer token for the XiaoZhi server (sent as
// "Authorization: Bearer <token>" on the WS upgrade). May be empty.
static const ble_uuid128_t kXiaozhiTokenUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x14, 0xa0, 0xf0, 0xe3);
// FaceConfig — encrypted compact JSON describing the avatar face tuning
// (eye/eyebrow/mouth geometry + face/background colours). WRITE applies live
// (no reboot) via the registered FaceConfigSink and stages the value for Apply
// (NVS persist). READ returns the active JSON so the editor seeds its controls.
static const ble_uuid128_t kFaceConfigUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x15, 0xa0, 0xf0, 0xe3);
// Battery — encrypted READ-only snapshot of the base-board INA226: 5 bytes
// [bus voltage mV u16 LE][shunt current mA i16 LE][percent i8]. percent = -1
// and mV = 0xFFFF mean "unknown" (no INA226 / not yet sampled). The browser
// polls this (battery changes slowly) — no NOTIFY.
static const ble_uuid128_t kBatteryUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x16, 0xa0, 0xf0, 0xe3);
// BatteryGauge — encrypted 1-byte flag (0=hide, 1=show) controlling the
// top-left battery gauge overlay on the avatar screen. Staged + applied on the
// Apply reboot, like the other enable flags.
static const ble_uuid128_t kBatteryGaugeUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x17, 0xa0, 0xf0, 0xe3);
// StartupArpeggio — encrypted 1-byte flag (0=silent boot, 1=play). Toggles the
// C5–E5–G5 speaker sanity check at startup. Staged + applied on the Apply reboot.
static const ble_uuid128_t kStartupArpeggioUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x2e, 0xa0, 0xf0, 0xe3);
// ServoLimits — encrypted compact JSON of per-servo zero position + motion
// range. READ returns the active JSON; WRITE stages for Apply (reboot → servo
// task reads at startup). See main/servo_limits.hpp for the schema.
static const ble_uuid128_t kServoLimitsUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x18, 0xa0, 0xf0, 0xe3);
// ServoRangeMode — encrypted 1-byte flag (0=off, 1=on). WRITE flips the servo
// task into "torque off + poll present-position" so the user can move the head
// by hand while the settings UI captures per-axis zero / min / max. READ
// returns the active state. Ephemeral — not persisted.
static const ble_uuid128_t kServoRangeModeUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x19, 0xa0, 0xf0, 0xe3);
// ServoPositions — encrypted READ-only 4-byte snapshot of live raw positions
// while in range mode: [yaw_raw i16 LE][pitch_raw i16 LE]. -1 = unknown
// (servo absent / not yet read). The browser polls this; no NOTIFY.
static const ble_uuid128_t kServoPositionsUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x1a, 0xa0, 0xf0, 0xe3);
// BoardKind — encrypted READ-only 1-byte board variant indicator. Mirrors
// board::BoardKind: 0 = M5Base, 1 = TakaoBase, 2 = AtomNyan. Lets the web UI
// hide sections that don't apply on the current hardware (servo config on
// Atom-nyan, battery gauge where there's no battery, etc.).
static const ble_uuid128_t kBoardKindUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x1b, 0xa0, 0xf0, 0xe3);
// ServoEnabled — encrypted 1-byte flag (0=disabled, 1=enabled) controlling
// the NVS-persisted master servo switch. When disabled, the servo VM rail
// stays off at boot and the servo task is never spawned. Distinct from the
// ServoRangeMode chr above (= runtime torque toggle, ephemeral). Staged +
// applied on the Apply reboot, like the other enable flags.
static const ble_uuid128_t kServoEnabledUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x1c, 0xa0, 0xf0, 0xe3);
// McpToken — encrypted **WRITE-ONLY** bearer for the /mcp/* (Claude Code
// Channel) HTTP API. WRITE stages the new token; Apply persists it to NVS
// and reboots. Empty body clears the NVS slot → /mcp/* answers 404 on the
// next boot (or falls back to CONFIG_MCP_API_TOKEN if it was set at build).
// READ is intentionally NOT exposed: the settings UI's "has_mcp_token" is
// surfaced via the existing Status notification instead.
static const ble_uuid128_t kMcpTokenUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x1d, 0xa0, 0xf0, 0xe3);
// LtConfig — encrypted compact JSON for the LT timekeeper (talk length,
// warning threshold, repeat period, announcement words). WRITE applies live
// (no reboot) via the registered LtConfigSink and stages for Apply (NVS
// persist); READ returns the active JSON. See main/lt_timer.hpp.
static const ble_uuid128_t kLtConfigUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x1e, 0xa0, 0xf0, 0xe3);
// AudioMetrics — encrypted READ-only JSON snapshot of the last conversation
// turn's audio pipeline stats (decode time, recv→play latency, speaker
// queue depth, effective playback sps). Updated by main/conversation_task
// via SharedState::update_audio_metrics; the GATT callback reads the latest
// snapshot via the registered AudioMetricsJsonGetter. No NOTIFY — clients
// poll on demand. See main/conversation_task.cpp `log_playback_metrics()`.
static const ble_uuid128_t kAudioMetricsUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x1f, 0xa0, 0xf0, 0xe3);
// LedState — encrypted R/W. 5-byte payload [mode][R][G][B][brightness].
// READ returns the live SharedState atomics via the registered getter; WRITE
// applies through the LedStateSink which forwards to main/led_task's
// SharedState. Stays ephemeral (no NVS persistence) — the boot defaults
// (gradient @ ~10%) are the resting state across reboots.
static const ble_uuid128_t kLedStateUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x20, 0xa0, 0xf0, 0xe3);
// JttsIdleEnabled — encrypted 1-byte flag (0=disabled, 1=enabled). Master
// switch for the jtts idle babble (the avatar speaking random phrases when
// nothing else is going on). When this is off AND OpenAiEnabled is also off,
// the firmware activates a mic-driven lip-sync mode instead (the mouth tracks
// ambient sound level). Apply path stages + persists on reboot like the other
// enable flags.
static const ble_uuid128_t kJttsIdleEnabledUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x22, 0xa0, 0xf0, 0xe3);
// AvatarBytecode — encrypted R/W chunked transport for the avatar face
// bytecode (.avbc). The same payload that HTTP `POST /api/avatar-dsl` accepts,
// but framed for BLE so the bytecode can exceed the per-write AES-GCM
// plaintext cap (~484 B). READ returns a status JSON
// {"state":"idle|receiving|done|failed","received":N,"total":M,"error":"..."}.
// WRITE payload is `[op:u8][...]`:
//   op 0x00 begin : [total_size:u16 LE]  — clear accumulator, set expected
//   op 0x01 data  : [bytes...]           — append to accumulator
//   op 0x02 commit: ()                   — when accumulator.size() == total,
//                                          validate + persist NVS + apply sink
//   op 0xff reset : ()                   — clear NVS, sink(nullptr, 0) (revert
//                                          to firmware-embedded default)
static const ble_uuid128_t kAvatarBytecodeUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x21, 0xa0, 0xf0, 0xe3);
// MicLipGain — encrypted R/W. 4-byte payload [input_pct LE u16][output_pct
// LE u16]. Calibrates the mic-driven lip-sync task (main/mic_lip_sync_task.cpp)
// live without a reboot. Persisted via save_mic_lip_gain so the next boot
// replays the same calibration.
static const ble_uuid128_t kMicLipGainUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x23, 0xa0, 0xf0, 0xe3);
// LedMouthSyncEnabled — encrypted 1-byte flag. When 1, main/led_task blends
// the nekomimi brightness with SharedState::mouth_open so the ears pulse
// along with whatever's driving the avatar's mouth. Staged + persisted via
// the Apply path (same as bat_gauge_enabled / servo_enabled).
static const ble_uuid128_t kLedMouthSyncUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x24, 0xa0, 0xf0, 0xe3);
// OperationMode — encrypted 1-byte enum (0=MicLipSync, 1=JttsRandom,
// 2=Conversation). Single source of truth for the avatar's behaviour;
// the legacy openai_enabled / jtts_idle_enabled chrs are derived from it
// at boot and remain readable for backwards compat with older clients.
static const ble_uuid128_t kOperationModeUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x25, 0xa0, 0xf0, 0xe3);
// BargeInEnabled — encrypted 1-byte flag. When 1, a screen tap during AI
// reply playback fires barge_in_request; otherwise the tap is ignored.
// Defaults off (false positives from incidental touch were too easy).
static const ble_uuid128_t kBargeInEnabledUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x26, 0xa0, 0xf0, 0xe3);
// AudioOutput — encrypted 1-byte enum (0=Auto, 1=Internal, 2=ModuleAudio).
// Forces a specific output codec selection at next boot, overriding the
// boot-time Module Audio probe when set to Internal / ModuleAudio. Default
// Auto preserves the historical probe-and-route behaviour.
static const ble_uuid128_t kAudioOutputUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x29, 0xa0, 0xf0, 0xe3);
// LipSyncMode — encrypted 1-byte enum (0=Brightness, 1=LevelMeter).
// Selects the nekomimi LED renderer used while led_mouth_sync_enabled is
// true. Staged + applied on the next boot.
static const ble_uuid128_t kLipSyncModeUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x2a, 0xa0, 0xf0, 0xe3);
// MicLipAgcEnabled — encrypted 1-byte flag. When 1 (default),
// main/mic_lip_sync_task rebases the dBFS window on the tracked ambient
// noise floor (auto-compensates for rooms / mic gains); when 0, a fixed
// floor is used. Staged + applied on the next boot.
static const ble_uuid128_t kMicLipAgcUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x2b, 0xa0, 0xf0, 0xe3);
// SpeakerVolume — encrypted R/W. 2-byte payload [pct LE u16] (0..200,
// 100 = factory default). Live-applied via sink (no reboot).
static const ble_uuid128_t kSpeakerVolumeUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x2c, 0xa0, 0xf0, 0xe3);
// JttsSay — encrypted WRITE-only. UTF-8 (kana) text bytes; sink kicks
// off a worker task that runs jtts::synthesize + M5.Speaker.playRaw.
// Used by the settings page "話す" test button so the user can audition
// the jtts voice without the full MCP token gate /mcp/say requires.
static const ble_uuid128_t kJttsSayUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x2d, 0xa0, 0xf0, 0xe3);
// DeviceName — encrypted R/W UTF-8 string (up to 24 bytes). Operator-set
// override for the BLE advertising name AND the mDNS hostname seed (after
// RFC-1123 sanitization). Empty means "use auto-generated Stackchan-XXXXXX".
// Staged + applied on the next boot — the running advertising name can't be
// changed without restarting GAP.
static const ble_uuid128_t kDeviceNameUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x27, 0xa0, 0xf0, 0xe3);
// AuthPassword — encrypted **WRITE-ONLY** UTF-8 string (up to 64 bytes).
// When non-empty, SHA-256(password) is mixed into the HKDF salt on every
// session handshake (clients without the right password derive a different
// key → GCM tag fails on every encrypted op), and HTTP Basic Auth gates the
// settings web UI + every /api/*. Empty clears the gate (no auth). READ is
// intentionally NOT exposed — the value is stored plaintext in NVS but never
// returned over the wire; status surfaces only a has_* flag.
static const ble_uuid128_t kAuthPasswordUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x28, 0xa0, 0xf0, 0xe3);

// --- Mutable state guarded by g_mutex ---
static SemaphoreHandle_t g_mutex = nullptr;


static DeviceConfig g_active;
static StagedConfig g_staging;
static bool g_wifi_connected = false;
static uint16_t g_status_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_status_subscribed = false;

// Cached battery snapshot (guarded by g_mutex). -1 percent / mv mean unknown.
static int g_battery_mv = -1;
static int g_battery_ma = 0;
static int g_battery_pct = -1;

// Val handles written by NimBLE during GATT registration.
static uint16_t g_ssid_handle = 0;
static uint16_t g_pass_handle = 0;
static uint16_t g_key_handle = 0;
static uint16_t g_apply_handle = 0;
static uint16_t g_status_handle = 0;
static uint16_t g_kx_handle = 0;
static uint16_t g_enabled_handle = 0;
static uint16_t g_rtp_enabled_handle = 0;
static uint16_t g_jtts_idle_enabled_handle = 0;
static uint16_t g_bat_gauge_handle = 0;
static uint16_t g_boot_arp_handle = 0;
static uint16_t g_servo_enabled_handle = 0;
static uint16_t g_mcp_token_handle = 0;
static uint16_t g_lt_config_handle = 0;
static uint16_t g_servo_limits_handle = 0;
static uint16_t g_servo_range_mode_handle = 0;
static uint16_t g_servo_positions_handle = 0;
static uint16_t g_audio_metrics_handle = 0;
static AudioMetricsJsonGetter g_audio_metrics_getter = nullptr;
static uint16_t g_led_state_handle = 0;
static LedStateGetter g_led_state_getter = nullptr;
static LedStateSink g_led_state_sink = nullptr;
static uint16_t g_mic_lip_gain_handle = 0;
static uint16_t g_speaker_volume_handle = 0;
static SpeakerVolumeGetter g_speaker_volume_getter = nullptr;
static SpeakerVolumeSink   g_speaker_volume_sink   = nullptr;
static uint16_t g_jtts_say_handle = 0;
static JttsSayKanaSink g_jtts_say_sink = nullptr;
static MicLipGainGetter g_mic_lip_gain_getter = nullptr;
static MicLipGainSink g_mic_lip_gain_sink = nullptr;
static uint16_t g_led_mouth_sync_handle = 0;
static uint16_t g_operation_mode_handle = 0;
static uint16_t g_audio_output_handle = 0;
static uint16_t g_lip_sync_mode_handle = 0;
static uint16_t g_mic_lip_agc_handle = 0;
static uint16_t g_barge_in_enabled_handle = 0;
static uint16_t g_device_name_handle = 0;
static uint16_t g_auth_password_handle = 0;
static uint16_t g_avatar_bc_handle = 0;
static AvatarBytecodeSink g_avatar_bc_sink = nullptr;

// Avatar bytecode chunked-upload accumulator. Lives entirely in RAM (no
// flash partition writes during transfer — unlike OTA — because the whole
// blob fits in `avatar_vm::storage::kMaxBytecodeBytes` (32 KiB) and we want
// to validate before persisting). All four fields are guarded by g_mutex.
enum class AvatarBcState : std::uint8_t { Idle, Receiving, Done, Failed };
static AvatarBcState g_avatar_bc_state = AvatarBcState::Idle;
static std::size_t g_avatar_bc_total = 0; // expected total from `begin`
static std::vector<std::uint8_t> g_avatar_bc_accum;
static std::string g_avatar_bc_error; // last failure reason for status READ
static uint16_t g_board_kind_handle = 0;
// Board variant byte, set by main at boot before BLE comes online (so the
// first central read sees the correct value). Default M5Base preserves the
// pre-AtomS3R behaviour for any client connecting against firmware that
// hasn't yet pushed a value.
static std::uint8_t g_board_kind = 0;
static uint16_t g_jtts_handle = 0;
static uint16_t g_ota_ctrl_handle = 0;
static uint16_t g_ota_data_handle = 0;
static uint16_t g_provider_handle = 0;
static uint16_t g_gemini_key_handle = 0;
static uint16_t g_xiaozhi_url_handle = 0;
static uint16_t g_xiaozhi_token_handle = 0;
static uint16_t g_face_config_handle = 0;
static uint16_t g_battery_handle = 0;
static uint16_t g_wifi_ip_handle = 0;
static uint16_t g_wifi_mac_handle = 0;
static uint16_t g_audio_ctrl_handle = 0;
static uint16_t g_audio_data_handle = 0;
static uint16_t g_audio_credit_handle = 0;

// Registered sink, owned by main/audio_stream_sink. nullptr → audio streaming
// quietly drops on the floor. Reads from the GATT host task only, so a plain
// pointer suffices (set_audio_stream_sink isn't called from a hot path).
static const AudioStreamSink* g_audio_sink = nullptr;
// Tracks whether the current connection has an active begin() so we can fire
// on_abort() on disconnect even without an explicit end/abort command.
static bool g_audio_session_active = false;

// Live face-config callback, owned by main/app_main. nullptr → live updates are
// dropped (the value is still staged + persisted on Apply). Called from the
// GATT host task; the callback must not parse JSON there (small stack).
static FaceConfigSink g_face_config_sink = nullptr;
static LtConfigSink g_lt_config_sink = nullptr;

// Range-mode sink + live position getter, owned by main/app_main. nullptr
// callbacks make the corresponding READ/WRITE behave as if the feature is
// disabled (positions read as unknown, range-mode WRITE is dropped).
static ServoRangeModeSink g_servo_range_mode_sink = nullptr;
static ServoPositionsGetter g_servo_positions_getter = nullptr;
static bool g_servo_range_mode = false; // active state, mirrored for READ

// Largest plaintext payload we accept on a single write — chosen to fit the
// jtts config JSON comfortably. The encrypted wire form adds 12 (nonce) + 16
// (tag) bytes; the scratch buffer below is sized accordingly. NimBLE
// reassembles prepared writes into a single mbuf chain that ble_hs_mbuf_to_flat
// then drops into our buffer.
constexpr std::size_t kMaxJttsConfigBytes = 768;

// OTA firmware chunks are sent in 512-byte plaintext slices (~540 bytes on the
// wire after AES-GCM framing). Comfortably under the 996-byte plaintext cap
// implied by the 1024-byte access-cb scratch buffer.
constexpr std::size_t kMaxOtaChunkBytes = 768;

// Per-connection application-layer crypto session. Reset on disconnect by
// config_service.cpp so the next central re-runs the X25519 handshake.
static crypto::Session g_session;

// One-shot timer fires ~200 ms after Apply to allow ATT response to flush.
static esp_timer_handle_t g_restart_timer = nullptr;

// --- Helpers ---

std::array<uint8_t, 2> compute_status_locked()
{
    uint8_t flags = 0;
    if (!g_active.wifi_ssid.empty()) flags |= 0x01;
    if (!g_active.wifi_password.empty()) flags |= 0x02;
    if (!g_active.openai_api_key.empty()) flags |= 0x04;
    if (g_active.openai_enabled) flags |= 0x08;
    if (!g_active.gemini_api_key.empty()) flags |= 0x10;
    if (g_active.provider == Provider::Gemini) flags |= 0x20;
    return {flags, g_wifi_connected ? uint8_t{1} : uint8_t{0}};
}

void restart_cb(void* /*arg*/)
{
    ESP_LOGI(kTag, "restarting now");
    esp_restart();
}

// Current STA IPv4 as a dotted-decimal string, or empty when Wi-Fi is
// disconnected / netif not yet up. Looked up on-demand from the default STA
// netif so we don't have to keep a shared mirror in sync with esp_event.
std::string current_wifi_ip()
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) return {};
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return {};
    if (info.ip.addr == 0) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return std::string(buf);
}

std::string wifi_sta_mac()
{
    std::uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                  mac[3], mac[4], mac[5]);
    return std::string(buf);
}

// --- GATT access callback ---

// Append plaintext through the crypto session as
// [12B nonce][ciphertext][16B tag]. Returns false on internal failure.
static bool append_encrypted(struct os_mbuf* om, std::span<const std::uint8_t> plain)
{
    auto enc = g_session.encrypt(plain);
    if (!enc) return false;
    return os_mbuf_append(om, enc->data(), enc->size()) == 0;
}

// Caller must hold g_mutex. Renders the current avatar-bytecode upload state
// as the JSON document served by chr 0x21 READ.
std::string avatar_bc_status_json_locked()
{
    const char* state = "idle";
    switch (g_avatar_bc_state) {
    case AvatarBcState::Idle:      state = "idle"; break;
    case AvatarBcState::Receiving: state = "receiving"; break;
    case AvatarBcState::Done:      state = "done"; break;
    case AvatarBcState::Failed:    state = "failed"; break;
    }
    // Conservative buffer — error string is bounded by short avatar_vm error
    // names ("too_large", "write_failed" etc.).
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  R"({"state":"%s","received":%u,"total":%u,"error":"%s"})",
                  state,
                  static_cast<unsigned>(g_avatar_bc_accum.size()),
                  static_cast<unsigned>(g_avatar_bc_total),
                  g_avatar_bc_error.c_str());
    return std::string(buf);
}

static int gatt_access_cb(uint16_t /*conn_handle*/, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt* ctxt, void* /*arg*/)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // KeyExchange — plaintext bootstrap. Lazily generate the device's
        // ephemeral keypair on first read and return the 32-byte public key.
        if (attr_handle == g_kx_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            auto pub = g_session.ensure_device_keypair();
            xSemaphoreGive(g_mutex);
            if (!pub) {
                ESP_LOGW(kTag, "ensure_device_keypair failed");
                return BLE_ATT_ERR_UNLIKELY;
            }
            int rc = os_mbuf_append(ctxt->om, pub->data(), pub->size());
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        // AudioCredit — plaintext, no session. Returns the sink's current
        // free space as a uint32 LE so the sender can pace its no-response
        // AudioData writes (credit-based flow control). 0 if no sink / no
        // active session.
        if (attr_handle == g_audio_credit_handle) {
            std::uint32_t credit = 0;
            if (g_audio_sink != nullptr && g_audio_sink->credit != nullptr &&
                g_audio_session_active) {
                credit = g_audio_sink->credit();
            }
            std::array<std::uint8_t, 4> le{
                static_cast<std::uint8_t>(credit & 0xff),
                static_cast<std::uint8_t>((credit >> 8) & 0xff),
                static_cast<std::uint8_t>((credit >> 16) & 0xff),
                static_cast<std::uint8_t>((credit >> 24) & 0xff)};
            int rc = os_mbuf_append(ctxt->om, le.data(), le.size());
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        if (attr_handle == g_ssid_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::string ssid = g_active.wifi_ssid;
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(ssid.data()), ssid.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_status_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            auto st = compute_status_locked();
            const bool ok = append_encrypted(ctxt->om, {st.data(), st.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_enabled_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.openai_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_rtp_enabled_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.rtp_audio_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_jtts_idle_enabled_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.jtts_idle_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_led_mouth_sync_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.led_mouth_sync_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_lip_sync_mode_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = static_cast<std::uint8_t>(g_active.lip_sync_mode);
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_mic_lip_agc_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.mic_lip_agc_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_operation_mode_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = static_cast<std::uint8_t>(g_active.operation_mode);
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_audio_output_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = static_cast<std::uint8_t>(g_active.audio_output);
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_barge_in_enabled_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.barge_in_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_device_name_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::string name = g_active.device_name;
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(name.data()), name.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_bat_gauge_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.battery_gauge_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_boot_arp_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.startup_arpeggio_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_servo_enabled_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_active.servo_enabled ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_jtts_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::string json = g_active.jtts_config_json;
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_ota_ctrl_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::string json = ota::status_json();
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_provider_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = static_cast<std::uint8_t>(g_active.provider);
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_face_config_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::string json = g_active.face_config_json;
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_lt_config_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::string json = g_active.lt_config_json;
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_servo_limits_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::string json = g_active.servo_limits_json;
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_servo_range_mode_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_servo_range_mode ? 1 : 0;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_servo_positions_handle) {
            ServoPositionsView view{-1, -1};
            ServoPositionsGetter getter = g_servo_positions_getter;
            if (getter != nullptr) view = getter();
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint16_t y = static_cast<std::uint16_t>(view.yaw_raw);
            const std::uint16_t p = static_cast<std::uint16_t>(view.pitch_raw);
            const std::array<std::uint8_t, 4> payload{
                static_cast<std::uint8_t>(y & 0xff),
                static_cast<std::uint8_t>((y >> 8) & 0xff),
                static_cast<std::uint8_t>(p & 0xff),
                static_cast<std::uint8_t>((p >> 8) & 0xff)};
            const bool ok = append_encrypted(ctxt->om, {payload.data(), payload.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_led_state_handle) {
            LedState s{};
            LedStateGetter getter = g_led_state_getter;
            if (getter != nullptr) s = getter();
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            // 6-byte payload [mode][R][G][B][bright][period_ds]. Older
            // clients that read 5 bytes ignore the trailing period — newer
            // ones (settings.html) pick it up.
            const std::array<std::uint8_t, 6> payload{
                s.mode, s.r, s.g, s.b, s.brightness, s.gradient_period_ds};
            const bool ok = append_encrypted(ctxt->om, {payload.data(), payload.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_mic_lip_gain_handle) {
            MicLipGain g{100, 100};
            MicLipGainGetter getter = g_mic_lip_gain_getter;
            if (getter != nullptr) g = getter();
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            // 4-byte payload [input_pct LE u16][output_pct LE u16].
            const std::array<std::uint8_t, 4> payload{
                static_cast<std::uint8_t>(g.input_pct & 0xff),
                static_cast<std::uint8_t>((g.input_pct >> 8) & 0xff),
                static_cast<std::uint8_t>(g.output_pct & 0xff),
                static_cast<std::uint8_t>((g.output_pct >> 8) & 0xff)};
            const bool ok = append_encrypted(ctxt->om, {payload.data(), payload.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_speaker_volume_handle) {
            std::uint16_t pct = 100;
            SpeakerVolumeGetter getter = g_speaker_volume_getter;
            if (getter != nullptr) pct = getter();
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            // 2-byte payload [pct LE u16].
            const std::array<std::uint8_t, 2> payload{
                static_cast<std::uint8_t>(pct & 0xff),
                static_cast<std::uint8_t>((pct >> 8) & 0xff)};
            const bool ok = append_encrypted(ctxt->om, {payload.data(), payload.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_avatar_bc_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::string json = avatar_bc_status_json_locked();
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_audio_metrics_handle) {
            // Fetch the snapshot off-lock — the getter pulls from
            // SharedState's own mutex and we don't want to nest g_mutex
            // around it. The getter returns "{}" when no turn has finished.
            std::string json{};
            if (g_audio_metrics_getter != nullptr) {
                json = g_audio_metrics_getter();
            } else {
                json = "{}";
            }
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_board_kind_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const std::uint8_t byte = g_board_kind;
            const bool ok = append_encrypted(ctxt->om, {&byte, 1});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_battery_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            // [mV u16 LE][mA i16 LE][pct i8]. Unknown → mV = 0xFFFF, pct = -1.
            const std::uint16_t mv =
                g_battery_mv < 0 ? 0xFFFFu : static_cast<std::uint16_t>(g_battery_mv);
            const std::int16_t ma = static_cast<std::int16_t>(g_battery_ma);
            const std::int8_t pct = static_cast<std::int8_t>(g_battery_pct);
            const std::array<std::uint8_t, 5> payload{
                static_cast<std::uint8_t>(mv & 0xff),
                static_cast<std::uint8_t>((mv >> 8) & 0xff),
                static_cast<std::uint8_t>(static_cast<std::uint16_t>(ma) & 0xff),
                static_cast<std::uint8_t>((static_cast<std::uint16_t>(ma) >> 8) & 0xff),
                static_cast<std::uint8_t>(pct)};
            const bool ok = append_encrypted(ctxt->om, {payload.data(), payload.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_wifi_ip_handle) {
            // current_wifi_ip() touches esp_netif, not g_session — but the
            // session check + encrypt still has to happen under g_mutex.
            const std::string ip = current_wifi_ip();
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(ip.data()), ip.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (attr_handle == g_wifi_mac_handle) {
            const std::string mac = wifi_sta_mac();
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_UNLIKELY;
            }
            const bool ok = append_encrypted(
                ctxt->om,
                {reinterpret_cast<const std::uint8_t*>(mac.data()), mac.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Scratch buffer sized for the largest payload. JttsConfig
        // (kMaxJttsConfigBytes = 768) is currently the biggest; encrypted on
        // the wire adds 12 (nonce) + 16 (tag) bytes. NimBLE's prepared write
        // reassembly flattens long writes into this single mbuf chain.
        //
        // Static rather than on-stack: at 1 KiB it ate a quarter of the
        // NimBLE host task's 4 KiB stack, and OTA writes (cJSON parse +
        // SPI-flash work below) tipped it into a stack-overflow panic
        // ("A stack overflow in task nimble_host"). All GATT access cbs
        // run on the single NimBLE host task — no reentrancy, so a shared
        // static buffer is safe.
        static std::array<std::uint8_t, 1024> buf;
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf.data(),
                                      static_cast<uint16_t>(buf.size()), &out_len);
        if (rc != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;

        // KeyExchange WRITE completes the X25519 handshake. Plaintext, 32 bytes.
        if (attr_handle == g_kx_handle) {
            if (out_len != 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            auto result = g_session.complete_handshake(
                std::span<const std::uint8_t, 32>{buf.data(), 32});
            xSemaphoreGive(g_mutex);
            if (!result) {
                ESP_LOGW(kTag, "complete_handshake failed: %d", static_cast<int>(result.error()));
                // CryptoNotReady → handshake attempted before pubkey was read.
                return result.error() == Error::CryptoNotReady ? BLE_ATT_ERR_UNLIKELY
                                                                : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            return 0;
        }

        // Audio streaming intentionally bypasses the AES-GCM session. The
        // payload is ephemeral playback audio (no credentials, no PII), and
        // the AES-GCM overhead (~11% wire bytes + per-chunk CPU on both
        // sides) eats into the BLE bandwidth budget that the streaming
        // decoder is already tight on.
        if (attr_handle == g_audio_ctrl_handle) {
            if (out_len > kMaxJttsConfigBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const std::string cmd(reinterpret_cast<const char*>(buf.data()), out_len);
            ESP_LOGD(kTag, "audio_ctrl write: %u B sink=%p active=%d cmd='%.*s'",
                     static_cast<unsigned>(out_len),
                     static_cast<const void*>(g_audio_sink),
                     g_audio_session_active ? 1 : 0,
                     static_cast<int>(std::min<std::size_t>(out_len, 80)), cmd.c_str());
            const bool is_begin = cmd.find("\"begin\"") != std::string::npos;
            const bool is_end   = cmd.find("\"end\"")   != std::string::npos;
            const bool is_abort = cmd.find("\"abort\"") != std::string::npos;

            if (g_audio_sink != nullptr) {
                if (is_begin) {
                    std::uint32_t sr = 24000;
                    std::uint8_t ch = 1;
                    auto sr_pos = cmd.find("\"sample_rate\"");
                    if (sr_pos != std::string::npos) {
                        sr = std::strtoul(cmd.c_str() + cmd.find(':', sr_pos) + 1, nullptr, 10);
                    }
                    auto ch_pos = cmd.find("\"channels\"");
                    if (ch_pos != std::string::npos) {
                        ch = static_cast<std::uint8_t>(
                            std::strtoul(cmd.c_str() + cmd.find(':', ch_pos) + 1, nullptr, 10));
                    }
                    if (g_audio_sink->on_begin) g_audio_sink->on_begin(sr, ch);
                    g_audio_session_active = true;
                } else if (is_end) {
                    if (g_audio_sink->on_end) g_audio_sink->on_end();
                    g_audio_session_active = false;
                } else if (is_abort) {
                    if (g_audio_sink->on_abort) g_audio_sink->on_abort(/*user_initiated=*/true);
                    g_audio_session_active = false;
                }
            }
            return 0;
        }
        if (attr_handle == g_audio_data_handle) {
            // Write-without-response: the return code never reaches the
            // sender, so a full buffer can only be dropped here (logged
            // inside on_data). The sender avoids ever reaching that state by
            // polling AudioCredit and pacing — see the credit() handler.
            if (g_audio_sink != nullptr && g_audio_sink->on_data && g_audio_session_active) {
                g_audio_sink->on_data(buf.data(), out_len);
            }
            return 0;
        }

        // All other writes require an established session and carry
        // [12B nonce][ciphertext][16B tag]. Decrypt before validating lengths.
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        if (!g_session.is_established()) {
            xSemaphoreGive(g_mutex);
            return BLE_ATT_ERR_UNLIKELY;
        }
        auto pt_result = g_session.decrypt({buf.data(), out_len});
        xSemaphoreGive(g_mutex);
        if (!pt_result) {
            ESP_LOGW(kTag, "decrypt failed on handle=%d", attr_handle);
            return BLE_ATT_ERR_UNLIKELY;
        }
        const std::vector<std::uint8_t>& pt = *pt_result;

        if (attr_handle == g_ssid_handle) {
            if (pt.size() > 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("ssid", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_pass_handle) {
            if (pt.size() > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("password", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_key_handle) {
            if (pt.size() > 256) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("api-key", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_enabled_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("openai-enabled", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_rtp_enabled_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("rtp-enabled", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_jtts_idle_enabled_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("jtts-idle-enabled", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_led_mouth_sync_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("led-mouth-sync", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_lip_sync_mode_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            if (pt[0] > static_cast<std::uint8_t>(LipSyncMode::LevelMeter)) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("lip-sync-mode", pt[0]);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_mic_lip_agc_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("mic-lip-agc", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_operation_mode_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            if (pt[0] > static_cast<std::uint8_t>(OperationMode::Conversation)) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("operation-mode", pt[0]);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_audio_output_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            if (pt[0] > static_cast<std::uint8_t>(AudioOutput::ModuleAudio)) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("audio-output", pt[0]);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_barge_in_enabled_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("barge-in", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_device_name_handle) {
            if (pt.size() > 24) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("device-name", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_auth_password_handle) {
            // Write-only. Empty body clears the gate (no auth required).
            if (pt.size() > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("auth-password", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_bat_gauge_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("battery-gauge", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_boot_arp_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("startup-arpeggio", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_servo_enabled_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("servo-enabled", pt[0] != 0 ? 1 : 0);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_mcp_token_handle) {
            // 128 byte cap mirrors the HTTP handler; an empty write clears.
            if (pt.size() > 128) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("mcp-token", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_jtts_handle) {
            if (pt.size() > kMaxJttsConfigBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("jtts-config", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_ota_ctrl_handle) {
            // JSON command (begin/end/abort). Result is observable via the
            // next READ on this characteristic (returns ota::status_json()).
            if (pt.size() > kMaxJttsConfigBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string cmd(reinterpret_cast<const char*>(pt.data()), pt.size());
            (void)ota::handle_control_command(cmd);
            return 0;
        }
        if (attr_handle == g_ota_data_handle) {
            // Raw firmware chunk; passed straight to esp_ota_write.
            if (pt.size() > kMaxOtaChunkBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            (void)ota::handle_data_chunk({pt.data(), pt.size()});
            return 0;
        }
        if (attr_handle == g_provider_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            Provider p = Provider::OpenAi;
            if (pt[0] == static_cast<std::uint8_t>(Provider::Gemini)) p = Provider::Gemini;
            else if (pt[0] == static_cast<std::uint8_t>(Provider::XiaoZhi)) p = Provider::XiaoZhi;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_num("provider", static_cast<std::uint32_t>(p));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_gemini_key_handle) {
            if (pt.size() > 256) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("gemini-api-key", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_xiaozhi_url_handle) {
            if (pt.size() > 256) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("xiaozhi-url", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_xiaozhi_token_handle) {
            if (pt.size() > 256) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("xiaozhi-token", std::move(val));
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_face_config_handle) {
            if (pt.size() > kMaxJttsConfigBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("face-config", val); // stage for Apply (NVS persist)
            FaceConfigSink sink = g_face_config_sink;
            xSemaphoreGive(g_mutex);
            // Live apply (no reboot). Hand the raw JSON to the app; it must not
            // parse on this (small) host-task stack.
            if (sink != nullptr) sink(val);
            return 0;
        }
        if (attr_handle == g_lt_config_handle) {
            if (pt.size() > kMaxJttsConfigBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("lt-config", val); // stage for Apply (NVS persist)
            LtConfigSink sink = g_lt_config_sink;
            xSemaphoreGive(g_mutex);
            // Live apply: demo_loop polls the version and re-parses off this
            // (small) host-task stack.
            if (sink != nullptr) sink(val);
            return 0;
        }
        if (attr_handle == g_servo_limits_handle) {
            if (pt.size() > kMaxJttsConfigBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(reinterpret_cast<const char*>(pt.data()), pt.size());
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.set_str("servo-limits", std::move(val)); // stage for Apply (reboot reloads)
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_servo_range_mode_handle) {
            if (pt.size() != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const bool on = (pt[0] != 0);
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_servo_range_mode = on;
            ServoRangeModeSink sink = g_servo_range_mode_sink;
            xSemaphoreGive(g_mutex);
            if (sink != nullptr) sink(on);
            return 0;
        }
        if (attr_handle == g_led_state_handle) {
            // 5-byte legacy payload [mode][R][G][B][brightness], or 6-byte
            // extended payload that also carries [period_ds] for the
            // gradient revolution. Anything else is a malformed write. The
            // 5-byte form keeps the existing period as-is (we just don't
            // populate the optional in the patch).
            if (pt.size() != 5 && pt.size() != 6) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            LedStatePatch p{};
            p.mode = pt[0];
            p.r = pt[1];
            p.g = pt[2];
            p.b = pt[3];
            p.brightness = pt[4];
            if (pt.size() == 6) p.gradient_period_ds = pt[5];
            LedStateSink sink = g_led_state_sink;
            if (sink != nullptr) sink(p);
            return 0;
        }
        if (attr_handle == g_mic_lip_gain_handle) {
            // 4-byte payload [input_pct LE u16][output_pct LE u16]. Anything
            // else is a malformed write. Sink applies live (SharedState
            // atomics) + persists via save_mic_lip_gain.
            if (pt.size() != 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            MicLipGain g{};
            g.input_pct = static_cast<std::uint16_t>(pt[0]) |
                          (static_cast<std::uint16_t>(pt[1]) << 8);
            g.output_pct = static_cast<std::uint16_t>(pt[2]) |
                           (static_cast<std::uint16_t>(pt[3]) << 8);
            MicLipGainSink sink = g_mic_lip_gain_sink;
            if (sink != nullptr) sink(g);
            return 0;
        }
        if (attr_handle == g_speaker_volume_handle) {
            // 2-byte payload [pct LE u16] (0..200). Sink applies live +
            // persists via save_speaker_volume.
            if (pt.size() != 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::uint16_t pct = static_cast<std::uint16_t>(pt[0]) |
                                (static_cast<std::uint16_t>(pt[1]) << 8);
            if (pct > 200) pct = 200;
            SpeakerVolumeSink sink = g_speaker_volume_sink;
            if (sink != nullptr) sink(pct);
            return 0;
        }
        if (attr_handle == g_jtts_say_handle) {
            // UTF-8 (kana) bytes. Empty / too-long writes are rejected;
            // the sink owns the rest (spawns a worker that runs jtts +
            // M5.Speaker.playRaw). 192 B cap matches the existing
            // /mcp/say behaviour (kMaxMcpSayBytes).
            constexpr std::size_t kMaxJttsBytes = 192;
            if (pt.empty() || pt.size() > kMaxJttsBytes) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            std::string_view kana{reinterpret_cast<const char*>(pt.data()), pt.size()};
            JttsSayKanaSink sink = g_jtts_say_sink;
            if (sink != nullptr) sink(kana);
            return 0;
        }
        if (attr_handle == g_avatar_bc_handle) {
            // Avatar bytecode chunked upload. Wire framing: [op:u8][...].
            if (pt.empty()) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const std::uint8_t op = pt[0];
            const std::span<const std::uint8_t> payload{pt.data() + 1, pt.size() - 1};
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            switch (op) {
            case 0x00: { // begin
                if (payload.size() != 2) {
                    xSemaphoreGive(g_mutex);
                    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                }
                const std::size_t total =
                    static_cast<std::size_t>(payload[0]) |
                    (static_cast<std::size_t>(payload[1]) << 8);
                if (total == 0 || total > avatar_vm::storage::kMaxBytecodeBytes) {
                    g_avatar_bc_state = AvatarBcState::Failed;
                    g_avatar_bc_error = "too_large";
                    g_avatar_bc_total = total;
                    g_avatar_bc_accum.clear();
                    xSemaphoreGive(g_mutex);
                    return 0; // status JSON carries the error
                }
                g_avatar_bc_state = AvatarBcState::Receiving;
                g_avatar_bc_total = total;
                g_avatar_bc_error.clear();
                g_avatar_bc_accum.clear();
                g_avatar_bc_accum.reserve(total);
                xSemaphoreGive(g_mutex);
                return 0;
            }
            case 0x01: { // data
                if (g_avatar_bc_state != AvatarBcState::Receiving) {
                    g_avatar_bc_state = AvatarBcState::Failed;
                    g_avatar_bc_error = "no_session";
                    xSemaphoreGive(g_mutex);
                    return 0;
                }
                if (g_avatar_bc_accum.size() + payload.size() > g_avatar_bc_total) {
                    g_avatar_bc_state = AvatarBcState::Failed;
                    g_avatar_bc_error = "overflow";
                    xSemaphoreGive(g_mutex);
                    return 0;
                }
                g_avatar_bc_accum.insert(
                    g_avatar_bc_accum.end(), payload.begin(), payload.end());
                xSemaphoreGive(g_mutex);
                return 0;
            }
            case 0x02: { // commit
                if (g_avatar_bc_state != AvatarBcState::Receiving) {
                    g_avatar_bc_state = AvatarBcState::Failed;
                    g_avatar_bc_error = "no_session";
                    xSemaphoreGive(g_mutex);
                    return 0;
                }
                if (g_avatar_bc_accum.size() != g_avatar_bc_total) {
                    g_avatar_bc_state = AvatarBcState::Failed;
                    g_avatar_bc_error = "size_mismatch";
                    xSemaphoreGive(g_mutex);
                    return 0;
                }
                // Move the accumulator out from under the lock so save/sink
                // (which can take ms) doesn't block GATT reads.
                std::vector<std::uint8_t> body = std::move(g_avatar_bc_accum);
                g_avatar_bc_accum.clear();
                AvatarBytecodeSink sink = g_avatar_bc_sink;
                xSemaphoreGive(g_mutex);

                auto save_r = avatar_vm::storage::save(body);
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                if (!save_r) {
                    g_avatar_bc_state = AvatarBcState::Failed;
                    g_avatar_bc_error = avatar_vm::storage::to_string(save_r.error());
                    xSemaphoreGive(g_mutex);
                    return 0;
                }
                g_avatar_bc_state = AvatarBcState::Done;
                g_avatar_bc_error.clear();
                xSemaphoreGive(g_mutex);
                // Live apply outside the lock. Sink failure leaves NVS
                // populated (next boot picks up the new bytecode).
                if (sink != nullptr) (void)sink(body.data(), body.size());
                return 0;
            }
            case 0xff: { // reset to firmware-embedded default
                g_avatar_bc_accum.clear();
                g_avatar_bc_total = 0;
                AvatarBytecodeSink sink = g_avatar_bc_sink;
                xSemaphoreGive(g_mutex);

                auto clear_r = avatar_vm::storage::clear();
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                if (!clear_r) {
                    g_avatar_bc_state = AvatarBcState::Failed;
                    g_avatar_bc_error = avatar_vm::storage::to_string(clear_r.error());
                    xSemaphoreGive(g_mutex);
                    return 0;
                }
                g_avatar_bc_state = AvatarBcState::Idle;
                g_avatar_bc_error.clear();
                xSemaphoreGive(g_mutex);
                if (sink != nullptr) (void)sink(nullptr, 0);
                return 0;
            }
            default:
                xSemaphoreGive(g_mutex);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
        }
        if (attr_handle == g_apply_handle) {
            if (pt.empty()) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

            xSemaphoreTake(g_mutex, portMAX_DELAY);
            DeviceConfig merged = g_active;
            g_staging.merge_into(merged);
            xSemaphoreGive(g_mutex);

            auto result = store::save(merged);
            if (!result) {
                ESP_LOGE(kTag, "NVS save failed on Apply");
                return BLE_ATT_ERR_UNLIKELY;
            }
            ESP_LOGI(kTag, "config saved, scheduling restart in 200 ms");

            if (g_restart_timer == nullptr) {
                esp_timer_create_args_t args{};
                args.callback = restart_cb;
                args.name = "ble_restart";
                esp_timer_create(&args, &g_restart_timer);
            }
            esp_timer_start_once(g_restart_timer, 200'000); // 200 ms
            return 0;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

// --- GATT service table ---
// These arrays live in static storage for the lifetime of the GATT server.

// Field order must match struct ble_gatt_chr_def: uuid, access_cb, arg,
// descriptors, flags, min_key_size, val_handle — C++ designated initializers
// must be in declaration order.
//
// Confidentiality is provided by an application-layer X25519 + AES-256-GCM
// session (see crypto.hpp). BLE link encryption (_ENC flags) is intentionally
// not used — NimBLE Just Works + Windows Web Bluetooth was unreliable. The
// KeyExchange characteristic is the only plaintext one; all other reads and
// writes carry [12B nonce][ciphertext][16B tag] and require an established
// session. The SM config in config_service.cpp is dormant.
static ble_gatt_chr_def kChrs[] = {
    {
        .uuid = &kKeyExchangeUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_kx_handle,
    },
    {
        .uuid = &kSsidUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ssid_handle,
    },
    {
        .uuid = &kPassUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_pass_handle,
    },
    {
        .uuid = &kApiKeyUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_key_handle,
    },
    {
        .uuid = &kApplyUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_apply_handle,
    },
    {
        .uuid = &kStatusUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_status_handle,
    },
    {
        .uuid = &kOpenAiEnabledUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_enabled_handle,
    },
    {
        .uuid = &kRtpAudioEnabledUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_rtp_enabled_handle,
    },
    {
        .uuid = &kJttsIdleEnabledUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_jtts_idle_enabled_handle,
    },
    {
        .uuid = &kBatteryGaugeUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_bat_gauge_handle,
    },
    {
        .uuid = &kStartupArpeggioUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_boot_arp_handle,
    },
    {
        .uuid = &kServoEnabledUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_servo_enabled_handle,
    },
    {
        .uuid = &kMcpTokenUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_mcp_token_handle,
    },
    {
        .uuid = &kLtConfigUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_lt_config_handle,
    },
    {
        .uuid = &kServoLimitsUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_servo_limits_handle,
    },
    {
        .uuid = &kServoRangeModeUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_servo_range_mode_handle,
    },
    {
        .uuid = &kServoPositionsUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &g_servo_positions_handle,
    },
    {
        .uuid = &kAudioMetricsUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &g_audio_metrics_handle,
    },
    {
        .uuid = &kLedStateUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_led_state_handle,
    },
    {
        .uuid = &kAvatarBytecodeUuid.u,
        .access_cb = gatt_access_cb,
        // R = status JSON; W = chunked upload (begin / data / commit / reset).
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_avatar_bc_handle,
    },
    {
        .uuid = &kMicLipGainUuid.u,
        .access_cb = gatt_access_cb,
        // R = [input_pct LE u16][output_pct LE u16]; W = same. Live apply.
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_mic_lip_gain_handle,
    },
    {
        .uuid = &kSpeakerVolumeUuid.u,
        .access_cb = gatt_access_cb,
        // R / W = [pct LE u16] in 0..200 (100 = factory default). Live apply.
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_speaker_volume_handle,
    },
    {
        .uuid = &kJttsSayUuid.u,
        .access_cb = gatt_access_cb,
        // W only — UTF-8 (kana) bytes. Sink spawns the synth+playback
        // worker. No read (idempotent test trigger).
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_jtts_say_handle,
    },
    {
        .uuid = &kLedMouthSyncUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_led_mouth_sync_handle,
    },
    {
        .uuid = &kOperationModeUuid.u,
        .access_cb = gatt_access_cb,
        // R = current mode byte; W = stage new mode for Apply (reboot).
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_operation_mode_handle,
    },
    {
        .uuid = &kAudioOutputUuid.u,
        .access_cb = gatt_access_cb,
        // R = current audio_output byte (0=Auto, 1=Internal, 2=ModuleAudio);
        // W = stage new value for Apply (reboot).
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_audio_output_handle,
    },
    {
        .uuid = &kLipSyncModeUuid.u,
        .access_cb = gatt_access_cb,
        // R = current lip_sync_mode byte (0=Brightness, 1=LevelMeter);
        // W = stage new value for Apply (reboot).
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_lip_sync_mode_handle,
    },
    {
        .uuid = &kMicLipAgcUuid.u,
        .access_cb = gatt_access_cb,
        // R = current mic_lip_agc_enabled byte (0/1); W = stage new value
        // for Apply (reboot).
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_mic_lip_agc_handle,
    },
    {
        .uuid = &kBargeInEnabledUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_barge_in_enabled_handle,
    },
    {
        .uuid = &kDeviceNameUuid.u,
        .access_cb = gatt_access_cb,
        // R = current operator-set name (may be empty); W = stage new name
        // for Apply (reboot). Take effect requires reboot because BLE GAP
        // advertising name is set once at on_sync() time.
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_device_name_handle,
    },
    {
        .uuid = &kAuthPasswordUuid.u,
        .access_cb = gatt_access_cb,
        // Write-only: empty body clears, non-empty installs the password gate.
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_auth_password_handle,
    },
    {
        .uuid = &kBoardKindUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &g_board_kind_handle,
    },
    {
        .uuid = &kJttsConfigUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_jtts_handle,
    },
    {
        .uuid = &kOtaControlUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ota_ctrl_handle,
    },
    {
        .uuid = &kOtaDataUuid.u,
        .access_cb = gatt_access_cb,
        // Both response and no-response writes. WRITE_NO_RSP lets the
        // Web Bluetooth client pipeline ATT_WRITE_CMD frames without
        // waiting for each ATT_WRITE_RSP, which is the dominant
        // cost in the old OTA path (~one 30-50 ms RTT per chunk).
        // The application protocol does its own flow control by reading
        // the OtaControl status every N chunks.
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &g_ota_data_handle,
    },
    {
        .uuid = &kProviderUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_provider_handle,
    },
    {
        .uuid = &kGeminiApiKeyUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_gemini_key_handle,
    },
    {
        .uuid = &kXiaozhiUrlUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_xiaozhi_url_handle,
    },
    {
        .uuid = &kXiaozhiTokenUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_xiaozhi_token_handle,
    },
    {
        .uuid = &kFaceConfigUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_face_config_handle,
    },
    {
        .uuid = &kBatteryUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &g_battery_handle,
    },
    {
        .uuid = &kWifiIpUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &g_wifi_ip_handle,
    },
    {
        .uuid = &kWifiMacUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &g_wifi_mac_handle,
    },
    {
        .uuid = &kAudioCtrlUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_audio_ctrl_handle,
    },
    {
        .uuid = &kAudioDataUuid.u,
        .access_cb = gatt_access_cb,
        // WRITE_NO_RSP so the browser can pipeline chunks without an
        // ATT_WRITE_RSP round-trip per packet — the same trick OTA uses.
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &g_audio_data_handle,
    },
    {
        .uuid = &kAudioCreditUuid.u,
        .access_cb = gatt_access_cb,
        // READ-only credit window for pacing the no-response AudioData
        // writes (see the kAudioCreditUuid comment).
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &g_audio_credit_handle,
    },
    {} // terminator: uuid = nullptr
};

static ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = kChrs,
    },
    {} // terminator: type = BLE_GATT_SVC_TYPE_END (0)
};

} // namespace

void init(const DeviceConfig& active)
{
    g_mutex = xSemaphoreCreateMutex();
    g_active = active;

    // Install the password-derived HKDF salt before any central can connect.
    // Empty password leaves the salt empty (back-compat: handshake derives
    // the same key it always did, no auth gate).
    if (!active.auth_password.empty()) {
        std::array<std::uint8_t, 32> hash{};
        // SHA-256(password). is224=0 selects SHA-256 (vs SHA-224).
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(active.auth_password.data()),
                       active.auth_password.size(),
                       hash.data(), 0);
        g_session.set_hkdf_salt(std::span<const std::uint8_t>{hash});
        ESP_LOGI(kTag, "BLE auth gate ON (HKDF salt installed)");
    } else {
        g_session.set_hkdf_salt({});
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(kSvcs);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gatts_count_cfg: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(kSvcs);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gatts_add_svcs: %d", rc);
    }
}

uint16_t status_val_handle()
{
    return g_status_handle;
}

void set_subscribe(uint16_t conn_handle, bool subscribed)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_status_conn_handle = conn_handle;
    g_status_subscribed = subscribed;
    xSemaphoreGive(g_mutex);
    ESP_LOGD(kTag, "Status CCCD: conn=%d subscribed=%d", conn_handle, subscribed ? 1 : 0);
}

void set_wifi_connected(bool connected)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_wifi_connected = connected;
    auto st = compute_status_locked();
    const bool subscribed = g_status_subscribed;
    const uint16_t conn_h = g_status_conn_handle;
    const uint16_t val_h = g_status_handle;
    // Encrypt under the lock so the session can't be reset mid-encrypt by
    // a disconnect from the host task.
    std::optional<std::vector<std::uint8_t>> wire;
    if (g_session.is_established()) {
        auto enc = g_session.encrypt({st.data(), st.size()});
        if (enc) wire = std::move(*enc);
    }
    xSemaphoreGive(g_mutex);

    if (!wire) {
        // Session not yet established — skip NOTIFY. The central will pull
        // the latest Status with a plain read after handshake.
        ESP_LOGD(kTag, "wifi notify skipped (session not established)");
        return;
    }
    if (subscribed && conn_h != BLE_HS_CONN_HANDLE_NONE && val_h != 0) {
        struct os_mbuf* om = ble_hs_mbuf_from_flat(wire->data(), wire->size());
        if (om != nullptr) {
            ble_gatts_notify_custom(conn_h, val_h, om);
        }
    }
}

void reset_session()
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_session.reset();
    // Any half-finished OTA must not survive a disconnect — esp_ota_abort
    // releases the partition so the bootloader keeps running the old image.
    ota::abort_update();
    // Same for in-flight audio streaming — abort so the decoder gets reset.
    // user_initiated = false because this is a transport-layer drop (BLE
    // disconnect), not the browser asking to abort. The sink may still
    // play any PCM it already accumulated as a graceful degradation.
    if (g_audio_sink != nullptr && g_audio_session_active && g_audio_sink->on_abort) {
        g_audio_sink->on_abort(/*user_initiated=*/false);
    }
    g_audio_session_active = false;
    // Drop any half-finished avatar bytecode upload so a reconnect starts
    // fresh — the partially received bytecode would be invalid anyway.
    g_avatar_bc_state = AvatarBcState::Idle;
    g_avatar_bc_total = 0;
    g_avatar_bc_accum.clear();
    g_avatar_bc_error.clear();
    xSemaphoreGive(g_mutex);
}

void set_audio_stream_sink(const AudioStreamSink* sink)
{
    g_audio_sink = sink;
}

void set_lt_config_sink(LtConfigSink sink)
{
    // Plain write (no g_mutex) — same rationale as set_face_config_sink
    // below: registered from app_main before gatt::init() creates the mutex.
    g_lt_config_sink = sink;
}

void set_face_config_sink(FaceConfigSink sink)
{
    // Plain write (no g_mutex): like set_audio_stream_sink, this may run before
    // gatt::init() creates the mutex, and it's not on a hot path. The WRITE
    // handler snapshots g_face_config_sink under g_mutex before calling it.
    g_face_config_sink = sink;
}

void set_servo_range_mode_sink(ServoRangeModeSink sink)
{
    g_servo_range_mode_sink = sink;
}

void set_servo_positions_getter(ServoPositionsGetter getter)
{
    g_servo_positions_getter = getter;
}

void set_audio_metrics_getter(AudioMetricsJsonGetter getter)
{
    // Plain write — registered at boot before NimBLE starts processing reads.
    g_audio_metrics_getter = getter;
}

void set_led_state_getter(LedStateGetter getter)
{
    g_led_state_getter = getter;
}

void set_led_state_sink(LedStateSink sink)
{
    g_led_state_sink = sink;
}

void set_avatar_bytecode_sink(AvatarBytecodeSink sink)
{
    g_avatar_bc_sink = sink;
}

void set_mic_lip_gain_getter(MicLipGainGetter getter)
{
    g_mic_lip_gain_getter = getter;
}

void set_mic_lip_gain_sink(MicLipGainSink sink)
{
    g_mic_lip_gain_sink = sink;
}

void set_speaker_volume_getter(SpeakerVolumeGetter getter)
{
    g_speaker_volume_getter = getter;
}

void set_speaker_volume_sink(SpeakerVolumeSink sink)
{
    g_speaker_volume_sink = sink;
}

void set_jtts_say_kana_sink(JttsSayKanaSink sink)
{
    g_jtts_say_sink = sink;
}

void set_board_kind(std::uint8_t kind)
{
    // Plain write, like the other sink setters — called at boot before BLE
    // serves any reads, so no mutex needed.
    g_board_kind = kind;
}

void set_battery(int millivolts, int milliamps, int percent)
{
    if (g_mutex == nullptr) return; // before gatt::init()
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_battery_mv = millivolts;
    g_battery_ma = milliamps;
    g_battery_pct = percent;
    xSemaphoreGive(g_mutex);
}

} // namespace stackchan::config::gatt
