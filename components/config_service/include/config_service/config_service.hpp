// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tl/expected.hpp>

namespace stackchan::config {

// Realtime conversation backend. Selects which provider's WebSocket the
// conversation task talks to, and which API key it picks up at boot.
enum class Provider : std::uint8_t {
    OpenAi = 0,   // OpenAI Realtime API (wss://api.openai.com/v1/realtime)
    Gemini = 1,   // Google Gemini Live API (wss://generativelanguage.googleapis.com/...)
    XiaoZhi = 2,  // XiaoZhi AI server (ws://<host>:8000/xiaozhi/v1/)
};

struct DeviceConfig {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string openai_api_key;
    std::string gemini_api_key;
    // XiaoZhi AI server: full WebSocket endpoint (ws://<host>:8000/xiaozhi/v1/)
    // and optional bearer token. Used only when provider == XiaoZhi; an empty
    // URL disables the conversation task for that provider.
    std::string xiaozhi_url;
    std::string xiaozhi_token;
    Provider provider = Provider::OpenAi;
    // Master switch for the OpenAI Realtime conversation task. Independent
    // of openai_api_key so the key can stay persisted while the feature is
    // turned off (saves data, lets the user take Stack-chan offline without
    // losing setup). Defaults to true for backwards compatibility with
    // existing NVS contents that pre-date this flag.
    bool openai_enabled = true;
    // Master switch for the Wi-Fi RTP live-audio receiver (main/wifi_audio.cpp).
    // When off, the UDP/RTP listener is not started at boot. Defaults to true
    // for backwards compatibility with NVS contents that pre-date this flag.
    // Independent of openai_enabled, though the receiver also self-disables
    // while conversation mode is on (they contend for the I2S bus + CPU).
    bool rtp_audio_enabled = true;
    // Master switch for the jtts idle babble (synthesised phrases the avatar
    // speaks while it has nothing else going on). When off, demo_loop skips
    // the synthesise/play step entirely. The phrase list and voice tuning
    // (jtts_config_json) are kept untouched so flipping the switch back on
    // restores the user's setup. Defaults to true (legacy behaviour).
    // When this is off AND openai_enabled is also off, demo_loop activates
    // a mic-driven lip-sync mode instead — see main/lip_sync_task.cpp.
    bool jtts_idle_enabled = true;
    // JSON document carrying the user-tunable jtts babble parameters and
    // phrase list. The producer (BLE client) writes the raw JSON; the
    // consumer (main/speech.cpp) parses on startup. Empty → compile-time
    // defaults. Capped at ~768 bytes plain text on the wire.
    std::string jtts_config_json;
    // Conversation system prompt / persona for the OpenAI / Gemini backends
    // (XiaoZhi's server owns its own persona, so it's ignored there). Empty →
    // the firmware's built-in default instructions. Settable over Wi-Fi.
    std::string system_prompt;
    // Extra HTTP headers attached to the conversation API's WebSocket upgrade
    // (all providers), e.g. a Cloudflare Access service token in front of a
    // proxied endpoint. Newline-separated "Name: value" lines. Settable over Wi-Fi.
    std::string conv_extra_headers;
    // Compact JSON describing the avatar face tuning (eye/eyebrow/mouth geometry
    // + face/background colours), as written by the settings UI's avatar editor.
    // Empty → built-in default face. Applied live over BLE and restored at boot.
    // See main/face_config.hpp for the schema.
    std::string face_config_json;
    // Show the battery gauge in the top-left of the avatar screen. Defaults to
    // true for backwards compatibility with NVS contents that pre-date the flag.
    // Takes effect after the Apply reboot.
    bool battery_gauge_enabled = true;
    // Master servo enable. When false, the servo VM rail stays off at boot AND
    // the servo task is never spawned (i.e. the head stays completely silent
    // and limp). Distinct from SharedState::servo_enabled which is the runtime
    // torque toggle (the on-device 操作 page). Defaults to true. Takes effect
    // after the Apply reboot.
    bool servo_enabled = true;
    // Bearer token for the /mcp/* (Claude Code Channel) HTTP API. Empty →
    // the entire namespace responds 404 (API disabled). Settable from BLE
    // / Wi-Fi (write-only). Takes effect after the Apply reboot. When
    // empty, the firmware falls back to CONFIG_MCP_API_TOKEN at boot so
    // pre-NVS installs keep working until rotated.
    std::string mcp_api_token;
    // Compact JSON for the LT timekeeper (default talk length, warning
    // threshold, repeat period, announcement texts + kana readings). Empty →
    // firmware defaults. Applied live on BLE/HTTP write and restored at
    // boot. See main/lt_timer.hpp for the schema.
    std::string lt_config_json;
    // Compact JSON describing per-servo zero position (raw SCS step) and motion
    // range (degrees relative to zero). Empty → built-in M5-base defaults. The
    // Takao base mounts the head differently so these are configurable + saved.
    // Takes effect after the Apply reboot. See main/servo_limits.hpp for the schema.
    std::string servo_limits_json;
    // Nekomimi NeoPixel live state — applied to SharedState at boot and
    // refreshed on every BLE chr 0x20 / HTTP POST write. Defaults mirror
    // SharedState's fallbacks (gradient mode, ~10% brightness).
    std::uint8_t led_mode = 3;          // 0=off, 1=solid, 2=breath, 3=gradient
    std::uint32_t led_color = 0x00404040u;
    std::uint8_t led_brightness = 26;
    std::uint8_t led_gradient_period_ds = 60;  // 6.0 s default (legacy hardcode)
};

enum class Error {
    NvsInit,
    NvsWrite,
    NimbleInit,
    GapAdvStart,
    GattRegister,
    // Application-layer crypto session (X25519 + AES-256-GCM).
    CryptoNotReady, // operation attempted before key exchange completed
    CryptoBadKey,   // peer pubkey rejected / ECDH failed
    CryptoAuth,     // AES-GCM authentication tag mismatch
    CryptoRng,      // ctr_drbg seeding / random failure
};

// Read device config from NVS namespace "stackchan_cfg". Missing keys → empty string.
DeviceConfig load();

// Start NimBLE host + GATT server + advertising. Non-fatal on failure: caller logs and continues.
tl::expected<void, Error> start(const DeviceConfig& current);

// Update Wi-Fi connectivity status; sends Status NOTIFY if a client is subscribed.
// Thread-safe — may be called from any task.
void notify_wifi_connected(bool connected);

// Update the cached battery snapshot served by the Battery READ characteristic.
// millivolts / percent < 0 mean "unknown" (no INA226 / not yet read). Thread-safe.
void notify_battery(int millivolts, int milliamps, int percent);

// Tell the settings service which hardware variant we booted on. The byte
// value mirrors board::BoardKind (0=M5Base, 1=TakaoBase, 2=AtomNyan) and is
// surfaced via the BoardKind READ characteristic so the web UI can hide
// sections that don't apply (e.g. servo configuration on Atom-nyan). Set
// once at boot before BLE comes online.
void set_board_kind(std::uint8_t kind);

// True while a BLE central is connected. Thread-safe; for status display.
bool ble_connected();

// Audio playback sink. The BLE settings service streams PCM16 LE mono
// chunks to whichever sink is registered here (set up at boot from
// main/audio_stream_sink.cpp). Callbacks run on the NimBLE host task,
// so the sink should hand off to a worker thread rather than blocking
// inline.
struct AudioStreamSink {
    void (*on_begin)(std::uint32_t sample_rate, std::uint8_t channels);
    // Returns true if the chunk was accepted (buffered whole), false if the
    // sink's buffer was full and the chunk was dropped. AudioData uses
    // write-WITHOUT-response, so this return value can't propagate back to
    // the sender — it's a device-side diagnostic only. Real flow control is
    // credit-based (see credit() below): the sender polls the sink's free
    // space and never has more bytes outstanding than it last saw free, so
    // on_data should never actually have to drop. The sink must accept a
    // chunk whole or not at all (a partial accept would split an ADTS frame
    // and desync the decoder).
    bool (*on_data)(const std::uint8_t* pcm16le, std::size_t bytes);
    void (*on_end)();
    // on_abort is invoked from two distinct paths: explicit op:'abort'
    // from the browser (user_initiated == true) and BLE disconnect
    // teardown (user_initiated == false). The sink can use this flag to
    // decide whether to discard partially-received data or fall through
    // to playback (graceful-degraded recovery from a dropped link).
    void (*on_abort)(bool user_initiated);
    // Bytes the sink can accept right now without dropping. Exposed via a
    // READ characteristic (AudioCredit) for credit-based flow control over
    // the no-response AudioData writes: the sender reads this, sends up to
    // that many bytes, then reads again. Because the sink has a single
    // producer (the BLE host) and only ever drains between reads, free space
    // measured at a read is a safe lower bound for the writes that follow —
    // the sender never overflows it. Returns 0 when no session is active.
    std::uint32_t (*credit)();
};

// Register the audio sink. nullptr unregisters. Last writer wins; not
// thread-safe to call concurrently with the GATT host task.
void set_audio_stream_sink(const AudioStreamSink* sink);

// Live avatar face-tuning callback. Invoked from the GATT host task on every
// FaceConfig WRITE with the raw (decrypted) JSON, so the application can apply
// it without waiting for Apply/reboot. The callback must be cheap and must NOT
// parse on the host task (its stack is small) — hand the string to a worker /
// shared state and parse there. nullptr unregisters.
using FaceConfigSink = void (*)(std::string_view json);
void set_face_config_sink(FaceConfigSink sink);

// LT timekeeper config JSON sink — same live-apply contract as FaceConfigSink.
using LtConfigSink = void (*)(std::string_view json);
void set_lt_config_sink(LtConfigSink sink);

// Servo range-setting mode: writing 1 puts the servo task into "torque off +
// poll present-position" mode so the user can move the head by hand and the
// settings UI can capture per-axis zero / min / max. Writing 0 returns to
// normal control. Ephemeral — not persisted to NVS. nullptr unregisters.
using ServoRangeModeSink = void (*)(bool on);
void set_servo_range_mode_sink(ServoRangeModeSink sink);

// Current SCS present-position (raw step, 0..1023) of each axis. -1 = unknown
// (servo absent / not yet read since entering range mode). Polled by the
// settings UI to display live values while the user moves the head by hand.
struct ServoPositionsView {
    std::int16_t yaw_raw;
    std::int16_t pitch_raw;
};
using ServoPositionsGetter = ServoPositionsView (*)();
void set_servo_positions_getter(ServoPositionsGetter getter);

// Audio pipeline metrics — returns the latest per-turn snapshot rendered as
// JSON for both BLE (chr 0x1f) and HTTP (/api/metrics/audio). The getter
// should not block; it's invoked from the GATT host task on read. Returning
// "{}" when no turn has completed yet is fine. nullptr unregisters.
using AudioMetricsJsonGetter = std::string (*)();
void set_audio_metrics_getter(AudioMetricsJsonGetter getter);

// NeoPixel live state — `mode` matches main/led_task's kModeOff/Solid/Breath/
// Gradient (0..3); `color` is the 24-bit RGB used by Solid/Breath (Gradient
// ignores it); `brightness` is the 0..255 master gain. Wired through BLE
// chr 0x20 + HTTP /api/led-state. Both read (current values) and write
// (apply new values) are mediated through the sink/getter — main owns the
// SharedState atomics and just exposes a thin closure.
struct LedState {
    std::uint8_t mode;        // 0=off, 1=solid, 2=breath, 3=gradient
    std::uint8_t r, g, b;     // RGB components (ignored when mode=gradient)
    std::uint8_t brightness;  // 0..255 master gain
    // Gradient revolution period in tenths of a second (1..255 → 0.1..25.5 s).
    // Only meaningful in mode=gradient.
    std::uint8_t gradient_period_ds;
};
// nullopt fields = "leave as-is". Apply order matters at the wire level so
// the caller can update brightness alone without disturbing the mode/colour.
struct LedStatePatch {
    std::optional<std::uint8_t> mode;
    std::optional<std::uint8_t> r, g, b;
    std::optional<std::uint8_t> brightness;
    std::optional<std::uint8_t> gradient_period_ds;
};
using LedStateGetter = LedState (*)();
using LedStateSink = void (*)(const LedStatePatch& patch);
void set_led_state_getter(LedStateGetter getter);
void set_led_state_sink(LedStateSink sink);

// Avatar face bytecode live-apply sink — fires after a complete `.avbc` has
// arrived over BLE chr 0x21 (op=commit) and has been validated + persisted
// to NVS by config_service. len=0 (data=nullptr) means "revert to firmware
// default" (after a successful reset op). Mirrors the HTTP sink declared in
// wifi_config_service.hpp so main can register the same closure with both
// transports. Returns true on success; failure leaves the new bytecode
// persisted but not applied (next reboot will pick it up).
using AvatarBytecodeSink = bool (*)(const std::uint8_t* data, std::size_t len);
void set_avatar_bytecode_sink(AvatarBytecodeSink sink);

} // namespace stackchan::config
