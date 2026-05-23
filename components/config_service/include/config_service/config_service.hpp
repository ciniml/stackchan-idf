// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <string>
#include <tl/expected.hpp>

namespace stackchan::config {

// Realtime conversation backend. Selects which provider's WebSocket the
// conversation task talks to, and which API key it picks up at boot.
enum class Provider : std::uint8_t {
    OpenAi = 0,   // OpenAI Realtime API (wss://api.openai.com/v1/realtime)
    Gemini = 1,   // Google Gemini Live API (wss://generativelanguage.googleapis.com/...)
};

struct DeviceConfig {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string openai_api_key;
    std::string gemini_api_key;
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
    // JSON document carrying the user-tunable jtts babble parameters and
    // phrase list. The producer (BLE client) writes the raw JSON; the
    // consumer (main/speech.cpp) parses on startup. Empty → compile-time
    // defaults. Capped at ~768 bytes plain text on the wire.
    std::string jtts_config_json;
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

} // namespace stackchan::config
