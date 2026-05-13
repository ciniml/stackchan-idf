// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "speech.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

#include <M5Unified.h>
#include <esp_timer.h>

namespace stackchan::app {

namespace {

constexpr float kTwoPi = 6.28318530718f;

// One syllable = a short tone with a smooth attack and release envelope.
struct Syllable {
    float frequency_hz;
    float duration_s;
    float gap_s; // silence after the syllable
};

// Cheap xorshift32 PRNG so syllables vary between calls.
class XorShift32 {
public:
    explicit XorShift32(std::uint32_t seed) noexcept : state_{seed != 0 ? seed : 0x12345678u} {}
    std::uint32_t next() noexcept
    {
        std::uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }
    float range(float low, float high) noexcept
    {
        const float u = static_cast<float>(next()) / static_cast<float>(0xFFFFFFFFu);
        return low + (high - low) * u;
    }
    int range_i(int low, int high) noexcept
    {
        return static_cast<int>(low + (next() % (high - low + 1)));
    }

private:
    std::uint32_t state_;
};

float envelope_curve(float t, float duration_s)
{
    // 20 ms attack, 60 ms release, flat plateau in the middle.
    constexpr float attack = 0.020f;
    constexpr float release = 0.060f;
    if (t < attack) {
        return t / attack;
    }
    if (t > duration_s - release) {
        const float r = (duration_s - t) / release;
        return std::clamp(r, 0.0f, 1.0f);
    }
    return 1.0f;
}

} // namespace

void Speech::babble(std::uint32_t seed)
{
    XorShift32 rng{seed};

    const int syllable_count = rng.range_i(3, 6);

    std::vector<Syllable> syllables;
    syllables.reserve(syllable_count);

    float total_s = 0.0f;
    for (int i = 0; i < syllable_count; ++i) {
        Syllable s{
            .frequency_hz = rng.range(220.0f, 480.0f),
            .duration_s = rng.range(0.12f, 0.22f),
            .gap_s = rng.range(0.04f, 0.10f),
        };
        total_s += s.duration_s + s.gap_s;
        syllables.push_back(s);
    }

    const std::size_t total_samples =
        static_cast<std::size_t>(total_s * static_cast<float>(kSampleRate)) + 1;
    pcm_.assign(total_samples, 0);

    constexpr float amplitude = 0.55f * 32767.0f;
    std::size_t cursor = 0;
    for (const auto& s : syllables) {
        const std::size_t s_samples =
            static_cast<std::size_t>(s.duration_s * static_cast<float>(kSampleRate));
        const float dphi = kTwoPi * s.frequency_hz / static_cast<float>(kSampleRate);
        float phase = 0.0f;
        for (std::size_t i = 0; i < s_samples && cursor + i < pcm_.size(); ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
            const float env = envelope_curve(t, s.duration_s);
            pcm_[cursor + i] = static_cast<std::int16_t>(amplitude * env * std::sin(phase));
            phase += dphi;
        }
        cursor += s_samples + static_cast<std::size_t>(s.gap_s * static_cast<float>(kSampleRate));
    }

    // Re-derive envelope from the PCM in fixed-size windows so the avatar's
    // mouth tracks any kind of audio, not just the syllables we just built.
    const std::size_t window =
        static_cast<std::size_t>(kSampleRate) * kEnvelopeStepMs / 1000u;
    const std::size_t windows = (pcm_.size() + window - 1) / window;
    envelope_.assign(windows, 0.0f);
    for (std::size_t w = 0; w < windows; ++w) {
        const std::size_t begin = w * window;
        const std::size_t end = std::min(begin + window, pcm_.size());
        std::int32_t peak = 0;
        for (std::size_t i = begin; i < end; ++i) {
            peak = std::max(peak, std::abs(static_cast<std::int32_t>(pcm_[i])));
        }
        envelope_[w] = static_cast<float>(peak) / 32767.0f;
    }

    duration_ms_.store(
        static_cast<std::uint32_t>(static_cast<float>(pcm_.size()) * 1000.0f /
                                   static_cast<float>(kSampleRate)),
        std::memory_order_relaxed);
    start_ms_.store(static_cast<std::uint32_t>(esp_timer_get_time() / 1000),
                    std::memory_order_release);

    M5.Speaker.playRaw(pcm_.data(), pcm_.size(), kSampleRate, /*stereo=*/false,
                       /*repeat=*/1, /*channel=*/-1,
                       /*stop_current_sound=*/true);
}

void Speech::stop()
{
    if (M5.Speaker.isPlaying()) {
        M5.Speaker.stop();
    }
    start_ms_.store(0, std::memory_order_release);
    duration_ms_.store(0, std::memory_order_release);
}

bool Speech::is_speaking() const
{
    const std::uint32_t start = start_ms_.load(std::memory_order_acquire);
    if (start == 0) {
        return false;
    }
    const std::uint32_t now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    return (now - start) < duration_ms_.load(std::memory_order_relaxed);
}

float Speech::current_mouth_open() const
{
    const std::uint32_t start = start_ms_.load(std::memory_order_acquire);
    if (start == 0 || envelope_.empty()) {
        return 0.0f;
    }
    const std::uint32_t now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    const std::uint32_t elapsed = now - start;
    if (elapsed >= duration_ms_.load(std::memory_order_relaxed)) {
        return 0.0f;
    }
    const std::size_t idx = elapsed / kEnvelopeStepMs;
    if (idx >= envelope_.size()) {
        return 0.0f;
    }
    return envelope_[idx];
}

} // namespace stackchan::app
