// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "mic_lip_sync_task.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "mic-lip";

// 16 kHz mono is plenty for envelope estimation — we throw away the spectral
// detail anyway. Each chunk is 256 samples = 16 ms, so the mouth opens at
// ~60 fps which more than covers the 30 fps render task.
constexpr std::uint32_t kSampleRate = 16'000;
constexpr std::size_t kChunkSamples = 256;
constexpr TickType_t kI2sSettle = pdMS_TO_TICKS(20);

// Adaptive envelope shaping (mirrors the simple approach in wifi_audio so the
// avatar looks consistent across sources). The smoothed envelope has a fast
// attack and slow release so a sudden sound opens the mouth quickly and lets
// it close gradually rather than chattering.
constexpr float kAttack = 0.55f;
constexpr float kRelease = 0.20f;
// Min/max RMS bounds for normalisation, in i16 units. Below kMinRms we treat
// the mic as silent (mouth closed); above kMaxRms the mouth is fully open.
// Tuned so room-ambient hiss stays at 0 and normal speaking distance peaks
// near 1.0 without the user having to shout.
constexpr float kMinRms = 200.0f;
constexpr float kMaxRms = 5000.0f;

float rms_amplitude(const std::int16_t* buf, std::size_t n)
{
    if (n == 0) return 0.0f;
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = static_cast<double>(buf[i]);
        acc += s * s;
    }
    return static_cast<float>(std::sqrt(acc / static_cast<double>(n)));
}

void mic_lip_sync_task_entry(void* arg)
{
    auto* state = static_cast<SharedState*>(arg);
    if (state == nullptr) {
        ESP_LOGE(kTag, "null state, aborting task");
        vTaskDelete(nullptr);
        return;
    }

    std::array<std::int16_t, kChunkSamples> buf{};
    bool mic_owned = false;
    float smoothed = 0.0f;

    ESP_LOGI(kTag, "mic lip-sync task started (rate=%u Hz, chunk=%u samples)",
             static_cast<unsigned>(kSampleRate), static_cast<unsigned>(kChunkSamples));

    for (;;) {
        // Speaker takes precedence: if any other producer (balloon say, MCP
        // say, jtts babble-in-emergency-fallback) starts playing, release
        // the mic so the I2S bus is free for the speaker. We pick the mic
        // back up once the speaker finishes.
        if (M5.Speaker.isPlaying()) {
            if (mic_owned) {
                M5.Mic.end();
                vTaskDelay(kI2sSettle);
                mic_owned = false;
                // Decay the smoothed envelope so the mouth closes while the
                // other producer drives `mouth_open` itself.
                smoothed = 0.0f;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!mic_owned) {
            M5.Speaker.end();
            vTaskDelay(kI2sSettle);
            if (!M5.Mic.begin()) {
                // No mic on this board — log once and back off. The retry
                // delay is long enough that we don't spam the log.
                ESP_LOGW(kTag, "M5.Mic.begin failed; retrying in 5 s");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            mic_owned = true;
        }

        if (!M5.Mic.record(buf.data(), buf.size(), kSampleRate, /*stereo=*/false)) {
            // Mic transitions are racy under heavy load (e.g. someone called
            // playRaw between our isPlaying check and record). Yield briefly
            // and let the next loop iteration redo the speaker check.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Wait for the queued buffer to finish capturing. The record call
        // queues asynchronously; isRecording() returns 0 once the DMA has
        // delivered the whole chunk.
        for (int i = 0; i < 100 && M5.Mic.isRecording(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        const float rms = rms_amplitude(buf.data(), buf.size());
        // Normalise to [0, 1] across the configured RMS window. The window
        // is wide enough to handle normal indoor speaking + room ambience.
        float level = (rms - kMinRms) / (kMaxRms - kMinRms);
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;

        // Attack/release smoothing so the mouth doesn't pop between every
        // sample window. Rising edges use the faster coefficient.
        const float coef = (level > smoothed) ? kAttack : kRelease;
        smoothed += (level - smoothed) * coef;

        state->mouth_open.store(smoothed, std::memory_order_relaxed);
    }
}

} // namespace

void start_mic_lip_sync_task(SharedState& state)
{
    // 4 KB internal RAM stack — plenty for a 512-byte mic buffer + sqrt loop
    // + M5Unified call frames. Pinned to core 1 alongside the render task so
    // it shares Core 0 with the (now disabled) conversation/audio paths.
    xTaskCreatePinnedToCore(mic_lip_sync_task_entry, "mic-lip", 4096, &state,
                            tskIDLE_PRIORITY + 2, nullptr, 1);
}

} // namespace stackchan::app
