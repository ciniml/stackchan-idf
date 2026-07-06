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

// Avatar's primary "what is the robot doing right now?" mode. Replaces the
// older pair of openai_enabled / jtts_idle_enabled toggles that gave the
// user 4 possible combinations (only 3 of which made sense). One enum, three
// mutually-exclusive modes:
//   MicLipSync   ‐ mic input drives the mouth; no jtts babble, no conversation
//   JttsRandom   ‐ random jtts phrases at idle; no conversation
//   Conversation ‐ realtime AI conversation (OpenAI / Gemini / XiaoZhi)
// app_main derives the legacy openai_enabled / jtts_idle_enabled gates from
// this at boot so the rest of the code path stays unchanged.
enum class OperationMode : std::uint8_t {
    MicLipSync   = 0,
    JttsRandom   = 1,
    Conversation = 2,
};

// Output audio routing on boards that can host an M5 Module Audio (M144)
// alongside an internal codec (currently CoreS3 = AW88298 internal,
// ES8388 line-out on the module):
//   Auto         ‐ honour the boot-time codec probe (use Module Audio when
//                  ES8388 ACKs at I2C 0x10, otherwise fall back to internal)
//   Internal     ‐ force the internal codec even if Module Audio is fitted
//                  (used when the module is plugged in for power-monitoring
//                  but the user wants to drive the on-board speaker)
//   ModuleAudio  ‐ force Module Audio; warn-and-fall-back-to-internal at
//                  boot if the codec is absent
// app_main.cpp evaluates `effective_audio_module` from this + the live
// probe result and uses it to gate the speaker pin override / ES8388 init
// / volume defaults.
enum class AudioOutput : std::uint8_t {
    Auto         = 0,
    Internal     = 1,
    ModuleAudio  = 2,
};

// How the nekomimi (cat-ear) LED strip should render the mouth_open signal
// when led_mouth_sync_enabled is true. Off (= led_mouth_sync_enabled false)
// is a separate gate, so this enum only covers the active-rendering choices:
//   Brightness  ‐ scale the strip brightness (current base animation
//                  intact, base brightness × mouth_open with a floor)
//   LevelMeter  ‐ overlay a discrete 5-step VU-meter: nekomimi LEDs are
//                  arranged as a triangle with LED #5 at the apex and LEDs
//                  #1 / #9 at the base; light pairs (1,9), (2,8), (3,7),
//                  (4,6) plus the apex (5) as the level rises 0 → 5
// Default Brightness keeps the historical behaviour.
enum class LipSyncMode : std::uint8_t {
    Brightness  = 0,
    LevelMeter  = 1,
};

// WIRE-FORMAT CONTRACT: the four enums above cross external boundaries as
// their numeric values — BLE characteristics (Provider / OperationMode /
// AudioOutput / LipSyncMode are 1-byte reads/writes from the web UI), the
// HTTP settings API, and the NVS u8 keys all carry the raw value. Never
// reorder or renumber; append new entries at the end only.
static_assert(static_cast<int>(Provider::OpenAi) == 0 &&
              static_cast<int>(Provider::Gemini) == 1 &&
              static_cast<int>(Provider::XiaoZhi) == 2,
              "Provider numbering is a BLE/HTTP/NVS wire contract — append only");
static_assert(static_cast<int>(OperationMode::MicLipSync) == 0 &&
              static_cast<int>(OperationMode::JttsRandom) == 1 &&
              static_cast<int>(OperationMode::Conversation) == 2,
              "OperationMode numbering is a BLE/HTTP/NVS wire contract — append only");
static_assert(static_cast<int>(AudioOutput::Auto) == 0 &&
              static_cast<int>(AudioOutput::Internal) == 1 &&
              static_cast<int>(AudioOutput::ModuleAudio) == 2,
              "AudioOutput numbering is a BLE/HTTP/NVS wire contract — append only");
static_assert(static_cast<int>(LipSyncMode::Brightness) == 0 &&
              static_cast<int>(LipSyncMode::LevelMeter) == 1,
              "LipSyncMode numbering is a BLE/HTTP/NVS wire contract — append only");

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
    // Play the startup arpeggio (C5–E5–G5) on boot — the quick speaker sanity
    // check immediately after audio init. Defaults to true (= ships behaviour
    // before the flag existed). Set to false for a silent boot. Takes effect
    // after the Apply reboot.
    bool startup_arpeggio_enabled = true;
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
    // Mic-driven lip sync calibration. Both as integer percent (10..1000) so
    // the wire format is plain u16 LE — easier to surface on a slider than a
    // float. Input gain multiplies the RMS amplitude before normalisation
    // (higher → more sensitive mic, useful on CoreS3 whose internal mic is
    // quieter than the AtomEcho's external one). Output gain scales the
    // final 0..1 mouth-open value (higher → wider mouth swings, clamped at
    // 1.0). Both default to "boost" so CoreS3 out-of-box gives a visible
    // mouth response; users can dial down on noisier boards.
    std::uint16_t mic_lip_input_gain_pct = 200;
    std::uint16_t mic_lip_output_gain_pct = 100;
    // Speaker output gain (integer percent, 0..200, 100 = factory default
    // per board). Live-applied without reboot via Speaker.setVolume(); the
    // sink also persists. 0 = mute. The actual M5Unified setVolume byte
    // is computed as min(255, board_base_volume * pct / 100) so existing
    // boards that already ship at 255 (StopWatch / Module Audio) just see
    // no change at 100% and clip at 255 above that.
    std::uint16_t speaker_volume_pct = 100;
    // Adaptive noise-floor AGC for the mic lip-sync envelope estimator.
    // When true (default), main/mic_lip_sync_task tracks the ambient
    // noise floor and rebases the mouth_open normalisation window on
    // top of it (auto-compensates for different rooms / mic gains).
    // When false, a fixed floor is used (legacy behaviour). Live atomic
    // — takes effect on the next mic frame without reboot.
    bool mic_lip_agc_enabled = true;
    // Modulate the nekomimi LED brightness with SharedState::mouth_open so the
    // ears pulse along with whatever's driving the avatar's mouth (mic
    // lip-sync, jtts babble, conversation playback, …). Off by default —
    // opt-in to keep the default look conservative. Implemented in
    // main/led_task.cpp: brightness = base + (max - base) * mouth_open.
    bool led_mouth_sync_enabled = false;
    // Lip-sync rendering style — only consulted when led_mouth_sync_enabled
    // is true. Brightness (default) preserves the prior behaviour;
    // LevelMeter switches to a 5-step VU-meter rendering up the cat-ear
    // triangle (base pair lit first, apex last). See LipSyncMode above.
    LipSyncMode lip_sync_mode = LipSyncMode::Brightness;
    // Primary operation mode. See OperationMode above. Defaults to
    // Conversation so a fresh install keeps the historical behaviour
    // (the legacy openai_enabled / jtts_idle_enabled toggles also default
    // to true, and the migration path in config_store::load preserves any
    // explicit override the user already saved before this field existed).
    OperationMode operation_mode = OperationMode::Conversation;
    // Audio output routing. Only meaningful when Module Audio is in play
    // (CoreS3 currently). Default Auto keeps existing devices on the
    // historical "probe → use whichever shows up" behaviour. Takes
    // effect after the Apply reboot. See AudioOutput above for the
    // semantics of each value.
    AudioOutput audio_output = AudioOutput::Auto;
    // Touch barge-in: when on, tapping the head during AI reply playback
    // interrupts the reply (so the user can cut in). The Si12T capacitive
    // pad on the head is sensitive enough that incidental contact (sleeve
    // brushing, hat fitting, putting Stack-chan back on the desk) fires
    // false barge-ins, so this defaults OFF — users opt in explicitly.
    // The nadenade stroke detector is independent of this flag and stays
    // active either way (it's a deliberate front→mid→back gesture).
    bool barge_in_enabled = false;
    // Operator-configurable device name. Becomes the BLE advertising name AND
    // the source for the mDNS hostname (after RFC-1123 sanitization: lowercase
    // alphanumerics + hyphen). Empty → fall back to the autogenerated
    // "Stackchan-XXXXXX" derived from the Wi-Fi STA MAC lower 3 bytes. Takes
    // effect on the next boot. Capped at 24 bytes plaintext so the full
    // advertising name + Stack-chan- prefix on hostname fits the BLE scan
    // response (31 byte adv frame budget).
    std::string device_name;
    // Pre-shared password gating access to the settings transports. When
    // non-empty:
    //   * BLE: SHA-256(password) is folded into the HKDF salt on every session
    //     handshake, so a client that connects without the right password
    //     derives a different AES key — every encrypted READ/WRITE then fails
    //     with a GCM tag mismatch. The UI surfaces the failure as
    //     "password incorrect".
    //   * HTTP: every API + the HTML page require HTTP Basic Auth, password
    //     compared to this value (username field is ignored). Browsers handle
    //     the 401 prompt natively.
    // Empty → no auth (back-compat with existing devices on trusted LANs).
    // Stored plaintext in NVS — anyone with flash dump access can recover it
    // anyway, and the on-device UI needs to know "is_set" for state display.
    // Capped at 64 bytes; never returned over the wire (only a has_* flag).
    std::string auth_password;
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

// Mic-driven lip-sync calibration: 2 × u16 integer-percent (10..1000 sane
// range, 100 = 1.0x). Wired through BLE chr 0x23 + HTTP /api/mic-lip-gain.
// READ returns the live SharedState atomics via the getter; WRITE applies
// through the sink which forwards into SharedState AND persists via
// save_mic_lip_gain (single-writer, no clobber from full save()).
struct MicLipGain {
    std::uint16_t input_pct;   // multiplier on mic RMS
    std::uint16_t output_pct;  // multiplier on final mouth-open value (clamped to 1.0)
};
using MicLipGainGetter = MicLipGain (*)();
using MicLipGainSink = void (*)(const MicLipGain& gain);
void set_mic_lip_gain_getter(MicLipGainGetter getter);
void set_mic_lip_gain_sink(MicLipGainSink sink);

// Speaker output volume: live u16 integer percent (0..200). BLE chr +
// HTTP /api/speaker-volume. READ returns the current live value via
// getter; WRITE applies through the sink which updates SharedState +
// M5.Speaker.setVolume() + persists via save_speaker_volume.
using SpeakerVolumeGetter = std::uint16_t (*)();
using SpeakerVolumeSink   = void (*)(std::uint16_t pct);
void set_speaker_volume_getter(SpeakerVolumeGetter getter);
void set_speaker_volume_sink(SpeakerVolumeSink sink);

// JTTS test-speak sink — UTF-8 (kana) bytes get spawned onto a worker
// task that synthesises + plays through M5.Speaker. Used by the BLE
// chr 0x2d and HTTP POST /api/jtts-say "speak this" buttons on the
// settings pages so the user can audition jtts voicing without the
// full MCP token path /mcp/say requires.
using JttsSayKanaSink = void (*)(std::string_view kana);
void set_jtts_say_kana_sink(JttsSayKanaSink sink);

// Application hooks shared by BOTH settings transports (the BLE GATT service
// here and the HTTP service in wifi_config_service). main builds this once at
// boot and hands the same struct to config::set_settings_hooks and
// wifi_config::set_settings_hooks, so each hook is written once instead of
// once per transport. nullptr fields unregister, same as the individual
// setters (which remain and are what the fan-out calls internally).
// face_config / lt_config are consumed by the BLE fan-out only — the HTTP
// service has no face-config route and wires its LT sink through the MCP
// registration path.
struct SettingsHooks {
    FaceConfigSink face_config = nullptr;
    LtConfigSink lt_config = nullptr;
    ServoRangeModeSink servo_range_mode = nullptr;
    ServoPositionsGetter servo_positions = nullptr;
    AudioMetricsJsonGetter audio_metrics = nullptr;
    LedStateGetter led_state_get = nullptr;
    LedStateSink led_state_set = nullptr;
    MicLipGainGetter mic_lip_gain_get = nullptr;
    MicLipGainSink mic_lip_gain_set = nullptr;
    SpeakerVolumeGetter speaker_volume_get = nullptr;
    SpeakerVolumeSink speaker_volume_set = nullptr;
    // Mirrors board::BoardKind; surfaced read-only so the web UIs can hide
    // sections that don't apply to the running hardware.
    std::uint8_t board_kind = 0;
};

// Register every hook in one call (BLE transport). Equivalent to calling the
// individual setters below plus set_board_kind.
void set_settings_hooks(const SettingsHooks& hooks);

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
