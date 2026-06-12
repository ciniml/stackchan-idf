// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <jtts/jtts.hpp>

namespace stackchan::app {

// Synthesises a short "babble" speech-like utterance and plays it through
// M5.Speaker. While the clip is playing, `current_mouth_open()` returns the
// instantaneous envelope (peak amplitude) of the audio, which the render
// task uses to drive the avatar's mouth.
class Speech {
public:
    // Sample rate of synthesised audio. 16 kHz int16.
    static constexpr std::uint32_t kSampleRate = 16'000;

    // Envelope window — one envelope sample covers this many ms of audio.
    static constexpr std::uint32_t kEnvelopeStepMs = 16;

    // Override the compile-time voice preset and phrase list with values from
    // a user-supplied JSON document (delivered over BLE, persisted in NVS).
    // Missing or invalid fields fall back to the defaults — invalid JSON is
    // ignored entirely. Call once at startup, before the first babble.
    void configure(const std::string& json);

    // Start a fresh utterance (non-blocking — M5.Speaker queues it).
    // `seed` selects which phrase to speak (seed % phrase count). Returns the
    // *display* text (発話内容) of the chosen phrase so the caller can show a
    // matching balloon — synthesis uses that phrase's separate *reading*
    // (発声内容, kana). Returns an empty string only when there are no phrases.
    std::string babble(std::uint32_t seed);

    // Speak an arbitrary kana string (発声内容; jtts has no kanji dictionary).
    // Same synthesis + envelope path as babble() so the avatar's mouth moves.
    // Cuts off any in-flight utterance. Returns false when synthesis failed
    // or the reading contained nothing speakable. Caller-side balloon text is
    // the caller's business (it usually differs from the reading).
    bool say(std::u32string_view reading);

    // Cancel any in-flight babble so we can hand the speaker / I2S bus to
    // someone else (e.g. mic loopback). After this is_speaking() returns false.
    void stop();

    // 0..1 envelope at "now". Returns 0 if nothing is playing.
    float current_mouth_open() const;

    bool is_speaking() const;

private:
    // Pre-computed envelope (peak amplitude per kEnvelopeStepMs window),
    // normalised to 0..1. Indexed by elapsed window count.
    std::vector<float> envelope_;
    // PCM kept alive while M5.Speaker plays it asynchronously.
    std::vector<std::int16_t> pcm_;

    // A single babble phrase. `display` (発話内容) is the UTF-8 text shown in
    // the balloon — free-form, may contain kanji/punctuation. `reading`
    // (発声内容) is the kana fed to jtts for synthesis (jtts has no kana-to-
    // phoneme dictionary, so kanji in a reading are silently skipped). The
    // two are decoupled on purpose so "こんにちは" can be read "こんにちわ".
    struct Phrase {
        std::string display;
        std::u32string reading;
    };

    // Voice preset + babble phrase list. configure() can overwrite these at
    // boot; otherwise they hold the compile-time defaults (Female child
    // preset, ~8 short Japanese phrases).
    jtts::Options opts_;
    std::vector<Phrase> phrases_;
    bool initialised_{false};

    std::atomic<std::uint32_t> start_ms_{0};
    std::atomic<std::uint32_t> duration_ms_{0};
};

} // namespace stackchan::app
