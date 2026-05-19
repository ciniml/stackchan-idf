// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "audio_stream_sink.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <esp_aac_dec.h>

#include "config_service/config_service.hpp"

namespace stackchan::app::audio_stream {

namespace {

constexpr const char* kTag = "audio-stream";

// AAC ADTS bytes inbound from BLE land here.
constexpr std::size_t kStreamBufferBytes = 32 * 1024;
constexpr std::size_t kStreamBufferTrigger = 256;

// Speaker channel reserved for BLE audio playback so it never collides
// with the conversation task's channel (0).
constexpr std::uint8_t kSpeakerChannel = 2;

// PSRAM-resident PCM ring buffer. ~21 s at 48 kHz mono in 2 MiB. Anything
// bigger and PSRAM realloc paths get fragile; anything smaller and a few
// hundred milliseconds of BLE jitter underflows the speaker.
constexpr std::size_t kRingSamples = 1024 * 1024;        // 2 MiB / int16 = 1 M samples
constexpr std::size_t kPrerollSamples = 48000 * 2;       // wait for 2 s before first playRaw
constexpr std::size_t kPlaybackChunkSamples = 1024;      // ~21 ms per chunk @ 48 kHz

// Rotating scratch buffers fed to M5.Speaker.playRaw. M5Unified doesn't
// copy the buffer immediately — it queues the pointer and the speaker
// task reads it asynchronously — so we cycle through N buffers to avoid
// overwriting samples still in flight.
constexpr std::size_t kScratchBuffers = 6;

// State accessed from the GATT host task (callbacks) and the worker task.
SharedState* g_state = nullptr;
StreamBufferHandle_t g_stream = nullptr;
TaskHandle_t g_worker = nullptr;

std::atomic<bool> g_end_requested{false};
std::atomic<bool> g_abort_requested{false};
std::atomic<bool> g_user_aborted{false};
std::atomic<bool> g_active{false};

// --- Callbacks (run on the NimBLE host task) ---

void on_begin(std::uint32_t sample_rate, std::uint8_t channels)
{
    g_end_requested.store(false, std::memory_order_release);
    g_abort_requested.store(false, std::memory_order_release);
    g_user_aborted.store(false, std::memory_order_release);
    g_active.store(true, std::memory_order_release);
    if (g_stream != nullptr) {
        xStreamBufferReset(g_stream);
    }
    if (g_state != nullptr) {
        g_state->audio_stream_active.store(true, std::memory_order_release);
    }
    if (g_worker != nullptr) {
        xTaskNotifyGive(g_worker);
    }
    ESP_LOGI(kTag, "begin: sr=%u ch=%u", static_cast<unsigned>(sample_rate),
             static_cast<unsigned>(channels));
}

void on_data(const std::uint8_t* data, std::size_t bytes)
{
    if (g_stream == nullptr || !g_active.load(std::memory_order_acquire)) return;
    xStreamBufferSend(g_stream, data, bytes, pdMS_TO_TICKS(200));
}

void on_end()
{
    g_end_requested.store(true, std::memory_order_release);
    if (g_worker != nullptr) xTaskNotifyGive(g_worker);
}

void on_abort(bool user_initiated)
{
    g_abort_requested.store(true, std::memory_order_release);
    if (user_initiated) {
        g_user_aborted.store(true, std::memory_order_release);
    }
    if (g_stream != nullptr) xStreamBufferReset(g_stream);
    if (g_worker != nullptr) xTaskNotifyGive(g_worker);
}

const config::AudioStreamSink kSink{
    .on_begin = &on_begin,
    .on_data = &on_data,
    .on_end = &on_end,
    .on_abort = &on_abort,
};

// --- PCM ring ---------------------------------------------------------

// Single-producer / single-consumer ring (both indices touched only by the
// worker task — no synchronization needed). Indices monotonically increase;
// physical position is `idx % capacity`. 32-bit overflow happens at
// ~25 hours of audio @ 48 kHz, well past anyone's patience.
struct Ring {
    std::int16_t* data = nullptr;
    std::size_t capacity = 0;
    std::uint32_t write = 0;
    std::uint32_t read = 0;

    std::size_t available_read() const { return write - read; }
    std::size_t available_write() const { return capacity - available_read(); }
    bool empty() const { return write == read; }

    // Append n samples. Caller must ensure available_write() >= n.
    void push(const std::int16_t* src, std::size_t n)
    {
        const std::size_t wpos = write % capacity;
        const std::size_t tail = std::min(n, capacity - wpos);
        std::memcpy(data + wpos, src, tail * sizeof(std::int16_t));
        if (n > tail) {
            std::memcpy(data, src + tail, (n - tail) * sizeof(std::int16_t));
        }
        write += n;
    }

    // Copy n samples into dst. Caller must ensure available_read() >= n.
    void pop(std::int16_t* dst, std::size_t n)
    {
        const std::size_t rpos = read % capacity;
        const std::size_t tail = std::min(n, capacity - rpos);
        std::memcpy(dst, data + rpos, tail * sizeof(std::int16_t));
        if (n > tail) {
            std::memcpy(dst + tail, data, (n - tail) * sizeof(std::int16_t));
        }
        read += n;
    }
};

// Drive mouth_open from the chunk peak.
void update_mouth(const std::int16_t* samples, std::size_t n)
{
    if (g_state == nullptr || n == 0) return;
    std::int32_t peak = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t v = std::abs(static_cast<std::int32_t>(samples[i]));
        if (v > peak) peak = v;
    }
    const float open = std::min(1.0f, static_cast<float>(peak) / 32767.0f * 1.5f);
    g_state->mouth_open.store(open, std::memory_order_relaxed);
}

// --- Worker ----------------------------------------------------------

void worker_task(void* /*arg*/)
{
    // PSRAM allocations — long-lived for the worker's lifetime.
    Ring ring{};
    ring.data = static_cast<std::int16_t*>(
        heap_caps_malloc(kRingSamples * sizeof(std::int16_t),
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ring.capacity = kRingSamples;

    constexpr std::size_t kRawCap = 4096;
    auto* raw_buf = static_cast<std::uint8_t*>(
        heap_caps_malloc(kRawCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    std::size_t pcm_frame_cap = 1024 * 2 * sizeof(std::int16_t) * 2;
    auto* pcm_frame = static_cast<std::uint8_t*>(
        heap_caps_malloc(pcm_frame_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    std::array<std::int16_t*, kScratchBuffers> scratches{};
    for (auto& s : scratches) {
        s = static_cast<std::int16_t*>(
            heap_caps_malloc(kPlaybackChunkSamples * sizeof(std::int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    std::size_t scratch_idx = 0;

    if (ring.data == nullptr || raw_buf == nullptr || pcm_frame == nullptr ||
        std::any_of(scratches.begin(), scratches.end(), [](auto* p) { return p == nullptr; })) {
        ESP_LOGE(kTag, "scratch alloc failed");
        vTaskDelete(nullptr);
        return;
    }

    void* decoder = nullptr;
    auto close_decoder = [&]() {
        if (decoder != nullptr) {
            esp_aac_dec_close(decoder);
            decoder = nullptr;
        }
    };

    std::uint32_t pcm_sample_rate = 0;
    bool playback_started = false;
    bool i2s_acquired = false;
    std::size_t raw_len = 0;

    auto teardown_session = [&]() {
        if (i2s_acquired) {
            M5.Speaker.stop(kSpeakerChannel);
            // Block until the speaker task actually drains its queue.
            while (M5.Speaker.isPlaying(kSpeakerChannel) > 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            M5.Speaker.end();
        }
        if (g_state != nullptr) {
            g_state->audio_stream_active.store(false, std::memory_order_release);
            g_state->mouth_open.store(0.0f, std::memory_order_relaxed);
        }
        close_decoder();
        ring.write = ring.read = 0;
        raw_len = 0;
        pcm_sample_rate = 0;
        playback_started = false;
        i2s_acquired = false;
        g_end_requested.store(false, std::memory_order_release);
        g_active.store(false, std::memory_order_release);
    };

    for (;;) {
        if (!g_active.load(std::memory_order_acquire)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }

        // ---- Per-session setup -----------------------------------------
        {
            esp_aac_dec_cfg_t cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
            cfg.no_adts_header = false;
            cfg.aac_plus_enable = false; // file is plain AAC-LC; HE-AAC mode
                                         // causes decode failures on the
                                         // post-FILL-element first audio frame.
            if (esp_aac_dec_open(&cfg, sizeof(cfg), &decoder) != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(kTag, "esp_aac_dec_open failed");
                teardown_session();
                continue;
            }
            ESP_LOGI(kTag,
                     "session start  INT free=%u largest=%u DMA-largest=%u  PSRAM free=%u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            ring.write = ring.read = 0;
            pcm_sample_rate = 0;
            playback_started = false;
            i2s_acquired = false;
            raw_len = 0;
        }

        // ---- Main streaming loop ---------------------------------------
        bool aborted = false;
        std::uint32_t debug_last_log_ms = 0;
        std::uint32_t frames_decoded = 0;
        std::uint32_t chunks_played = 0;

        while (g_active.load(std::memory_order_acquire)) {
            if (g_abort_requested.exchange(false, std::memory_order_acq_rel)) {
                aborted = true;
                ESP_LOGI(kTag, "abort");
                break;
            }

            // --- Pull more encoded bytes (non-blocking-ish) ---
            if (raw_len < kRawCap) {
                const std::size_t got = xStreamBufferReceive(
                    g_stream, raw_buf + raw_len, kRawCap - raw_len, pdMS_TO_TICKS(5));
                raw_len += got;
            }

            // --- Decode one frame if input + ring space allow ---
            // Need at least a plausible-frame's worth of input before
            // attempting decode, unless end was signalled (then we drain
            // the tail too).
            const bool end_pending = g_end_requested.load(std::memory_order_acquire);
            const bool stream_empty = xStreamBufferIsEmpty(g_stream);
            const bool enough_input = raw_len >= 512 || (end_pending && raw_len > 0);

            if (enough_input && ring.available_write() >= 2048) {
                esp_audio_dec_in_raw_t in{};
                in.buffer = raw_buf;
                in.len = raw_len;
                esp_audio_dec_out_frame_t out{};
                out.buffer = pcm_frame;
                out.len = pcm_frame_cap;
                esp_audio_dec_info_t info{};
                const auto rc = esp_aac_dec_decode(decoder, &in, &out, &info);

                if (rc == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    auto* bigger = static_cast<std::uint8_t*>(
                        heap_caps_realloc(pcm_frame, out.needed_size,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (bigger != nullptr) {
                        pcm_frame = bigger;
                        pcm_frame_cap = out.needed_size;
                    }
                } else if (rc != ESP_AUDIO_ERR_OK) {
                    // Resync. Only drop a byte if we're sure it's stale
                    // (lots of data in buffer). Otherwise wait for more.
                    if (in.consumed > 0) {
                        std::memmove(raw_buf, raw_buf + in.consumed,
                                     raw_len - in.consumed);
                        raw_len -= in.consumed;
                    } else if (raw_len >= 512 || end_pending) {
                        std::memmove(raw_buf, raw_buf + 1, raw_len - 1);
                        raw_len -= 1;
                    }
                } else {
                    // Decoded. Slide unconsumed tail to front.
                    if (in.consumed > 0 && in.consumed < raw_len) {
                        std::memmove(raw_buf, raw_buf + in.consumed,
                                     raw_len - in.consumed);
                    }
                    raw_len = (in.consumed > 0) ? (raw_len - in.consumed) : raw_len;

                    if (out.decoded_size > 0) {
                        const auto* samples = reinterpret_cast<const std::int16_t*>(out.buffer);
                        const std::uint8_t ch = info.channel ? info.channel : 1;
                        const std::size_t frames = out.decoded_size / sizeof(std::int16_t) / ch;
                        if (pcm_sample_rate == 0) {
                            pcm_sample_rate = info.sample_rate;
                            ESP_LOGI(kTag, "decoded format: %u Hz, %u ch source, %u kbps",
                                     static_cast<unsigned>(info.sample_rate),
                                     static_cast<unsigned>(ch),
                                     static_cast<unsigned>(info.bitrate / 1000));
                        }
                        // Mix-down to mono if needed (L channel only).
                        if (ch == 1) {
                            ring.push(samples, frames);
                        } else {
                            std::int16_t mono[1024];
                            const std::size_t step = std::min<std::size_t>(frames, 1024);
                            for (std::size_t i = 0; i < step; ++i) mono[i] = samples[i * ch];
                            ring.push(mono, step);
                        }
                        frames_decoded++;
                    }
                }
            }

            // --- Bring up the speaker once pre-roll is satisfied ---
            if (!playback_started && ring.available_read() >= kPrerollSamples) {
                // Wait for the conv-task to have actually released I2S.
                // It will have set conversation_yielded_i2s = true after
                // observing audio_stream_active in its main loop. 500 ms
                // is plenty given the conv-task polls every 100 ms.
                const auto wait_started = xTaskGetTickCount();
                while (g_state != nullptr &&
                       !g_state->conversation_yielded_i2s.load(std::memory_order_acquire)) {
                    if (xTaskGetTickCount() - wait_started > pdMS_TO_TICKS(500)) break;
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                M5.Speaker.begin();
                i2s_acquired = true;
                playback_started = true;
                ESP_LOGI(kTag, "playback started: %u samples pre-rolled @ %u Hz",
                         static_cast<unsigned>(ring.available_read()),
                         static_cast<unsigned>(pcm_sample_rate));
            }

            // --- Feed speaker as long as queue has space + ring has data ---
            if (playback_started && pcm_sample_rate > 0) {
                while (M5.Speaker.isPlaying(kSpeakerChannel) < 4 &&
                       ring.available_read() >= kPlaybackChunkSamples) {
                    auto* buf = scratches[scratch_idx];
                    scratch_idx = (scratch_idx + 1) % scratches.size();
                    ring.pop(buf, kPlaybackChunkSamples);
                    update_mouth(buf, kPlaybackChunkSamples);
                    M5.Speaker.playRaw(buf, kPlaybackChunkSamples, pcm_sample_rate,
                                       /*stereo=*/false, /*repeat=*/1, kSpeakerChannel,
                                       /*stop_current_sound=*/false);
                    chunks_played++;
                }
            }

            // --- Termination: end signalled + everything drained ---
            if (end_pending && stream_empty && raw_len == 0 && ring.empty() &&
                (!i2s_acquired || M5.Speaker.isPlaying(kSpeakerChannel) == 0)) {
                ESP_LOGI(kTag, "stream drained (decoded=%u, played=%u chunks)",
                         static_cast<unsigned>(frames_decoded),
                         static_cast<unsigned>(chunks_played));
                break;
            }

            // Periodic diagnostics — once every 5 seconds at DEBUG so we
            // can flip CONFIG_LOG_DEFAULT_LEVEL_DEBUG on if streaming
            // glitches and inspect ring / decoder / speaker queue depth.
            const std::uint32_t now_ms =
                static_cast<std::uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (now_ms - debug_last_log_ms > 5000) {
                ESP_LOGD(kTag,
                         "stream: raw=%uB ring=%uS spk_q=%d frames=%u chunks=%u%s",
                         static_cast<unsigned>(raw_len),
                         static_cast<unsigned>(ring.available_read()),
                         playback_started ? M5.Speaker.isPlaying(kSpeakerChannel) : -1,
                         static_cast<unsigned>(frames_decoded),
                         static_cast<unsigned>(chunks_played),
                         end_pending ? " end" : "");
                debug_last_log_ms = now_ms;
            }

            // Yield so IDLE isn't starved, especially when ring is empty
            // and we're waiting for BLE to deliver more bytes.
            if (ring.available_read() < kPlaybackChunkSamples) {
                vTaskDelay(pdMS_TO_TICKS(5));
            } else {
                taskYIELD();
            }
        }
        teardown_session();
        if (aborted) continue;
    }
}

} // namespace

void start(SharedState& state)
{
    ESP_LOGI(kTag, "start(): allocating stream buffer (%u B)",
             static_cast<unsigned>(kStreamBufferBytes));
    g_state = &state;

    g_stream = xStreamBufferCreateWithCaps(kStreamBufferBytes, kStreamBufferTrigger,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_stream == nullptr) {
        ESP_LOGE(kTag, "xStreamBufferCreateWithCaps failed");
        return;
    }

    if (xTaskCreatePinnedToCoreWithCaps(worker_task, "audio-stream", 8192, nullptr,
                                         tskIDLE_PRIORITY + 4, &g_worker, 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate audio-stream failed");
        return;
    }

    config::set_audio_stream_sink(&kSink);
    ESP_LOGI(kTag, "BLE audio stream sink registered (sink=%p)",
             static_cast<const void*>(&kSink));
}

} // namespace stackchan::app::audio_stream
