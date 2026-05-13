// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

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

    // Start a fresh utterance (non-blocking — M5.Speaker queues it).
    // `seed` randomises pitch / syllable count so successive calls differ.
    void babble(std::uint32_t seed);

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

    std::atomic<std::uint32_t> start_ms_{0};
    std::atomic<std::uint32_t> duration_ms_{0};
};

} // namespace stackchan::app
