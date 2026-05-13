// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "audio_codec.hpp"

#include <cstdlib>
#include <cstring>

#include <M5Unified.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_aac_dec.h"
#include "esp_aac_enc.h"
#include "esp_audio_dec.h"
#include "esp_audio_enc.h"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "aac";

bool ensure_aac_registered()
{
    static bool registered = false;
    if (registered) {
        return true;
    }
    if (esp_aac_enc_register() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "esp_aac_enc_register failed");
        return false;
    }
    if (esp_aac_dec_register() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "esp_aac_dec_register failed");
        return false;
    }
    registered = true;
    return true;
}

} // namespace

bool record_aac(std::uint32_t seconds, AacRecording& out)
{
    if (!ensure_aac_registered()) {
        return false;
    }

    out.sample_rate = 16'000;
    out.channels = 1;
    out.data.clear();

    // -- Open encoder ------------------------------------------------------
    esp_aac_enc_config_t aac_cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
    aac_cfg.sample_rate = static_cast<int>(out.sample_rate);
    aac_cfg.channel = ESP_AUDIO_MONO;
    aac_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    aac_cfg.bitrate = 32'000; // 32 kbps — sensible for 16 kHz mono speech
    aac_cfg.adts_used = true;

    esp_audio_enc_config_t enc_cfg{};
    enc_cfg.type = ESP_AUDIO_TYPE_AAC;
    enc_cfg.cfg = &aac_cfg;
    enc_cfg.cfg_sz = sizeof(aac_cfg);

    esp_audio_enc_handle_t enc = nullptr;
    if (esp_audio_enc_open(&enc_cfg, &enc) != ESP_AUDIO_ERR_OK || !enc) {
        ESP_LOGE(kTag, "esp_audio_enc_open failed");
        return false;
    }

    int in_size = 0, out_size = 0;
    if (esp_audio_enc_get_frame_size(enc, &in_size, &out_size) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "esp_audio_enc_get_frame_size failed");
        esp_audio_enc_close(enc);
        return false;
    }
    ESP_LOGI(kTag, "AAC encoder: in_frame=%d B, out_frame=%d B", in_size, out_size);

    // -- Record raw PCM straight from the mic ------------------------------
    // PCM lives in PSRAM (10 s × 16 kHz × 2 B = 320 KB).
    const std::size_t total_samples = out.sample_rate * seconds;
    std::vector<std::int16_t> pcm(total_samples, 0);

    M5.Speaker.end();
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!M5.Mic.record(pcm.data(), pcm.size(), out.sample_rate, /*stereo=*/false)) {
        ESP_LOGE(kTag, "M5.Mic.record failed");
        esp_audio_enc_close(enc);
        return false;
    }
    for (int i = 0; i < 50 && M5.Mic.isRecording() == 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    M5.Mic.end();
    vTaskDelay(pdMS_TO_TICKS(20));

    // -- Encode in-place chunk by chunk ------------------------------------
    std::vector<std::uint8_t> out_buf(out_size);
    esp_audio_enc_in_frame_t in_frame{};
    esp_audio_enc_out_frame_t out_frame{};
    out_frame.buffer = out_buf.data();
    out_frame.len = static_cast<std::uint32_t>(out_size);

    const std::size_t pcm_bytes = pcm.size() * sizeof(std::int16_t);
    out.data.reserve(static_cast<std::size_t>(aac_cfg.bitrate) * seconds / 8 + 1024);
    auto* const pcm_bytes_ptr = reinterpret_cast<std::uint8_t*>(pcm.data());

    for (std::size_t off = 0; off + static_cast<std::size_t>(in_size) <= pcm_bytes;
         off += in_size) {
        in_frame.buffer = pcm_bytes_ptr + off;
        in_frame.len = static_cast<std::uint32_t>(in_size);
        out_frame.encoded_bytes = 0;
        const auto ret = esp_audio_enc_process(enc, &in_frame, &out_frame);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(kTag, "esp_audio_enc_process failed: %d", static_cast<int>(ret));
            break;
        }
        out.data.insert(out.data.end(),
                        out_buf.data(),
                        out_buf.data() + out_frame.encoded_bytes);
    }

    esp_audio_enc_close(enc);
    ESP_LOGI(kTag, "encoded %u samples -> %u AAC bytes",
             static_cast<unsigned>(pcm.size()),
             static_cast<unsigned>(out.data.size()));
    return true;
}

bool play_aac(const AacRecording& recording)
{
    if (recording.data.empty()) {
        return false;
    }
    if (!ensure_aac_registered()) {
        return false;
    }

    // -- Open decoder ------------------------------------------------------
    esp_audio_dec_cfg_t dec_cfg{};
    dec_cfg.type = ESP_AUDIO_TYPE_AAC;
    dec_cfg.cfg = nullptr; // ADTS-framed input, no extra config needed
    dec_cfg.cfg_sz = 0;

    esp_audio_dec_handle_t dec = nullptr;
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(kTag, "esp_audio_dec_open failed");
        return false;
    }

    esp_audio_dec_in_raw_t raw{};
    raw.buffer = const_cast<std::uint8_t*>(recording.data.data());
    raw.len = static_cast<std::uint32_t>(recording.data.size());

    std::uint32_t frame_capacity = 4096;
    auto* frame_buf = static_cast<std::uint8_t*>(std::malloc(frame_capacity));
    if (!frame_buf) {
        esp_audio_dec_close(dec);
        return false;
    }

    // Decoded PCM is held in PSRAM (~320 KB for 10 s mono 16 kHz).
    std::vector<std::int16_t> pcm;
    pcm.reserve(static_cast<std::size_t>(recording.sample_rate) * 12);

    esp_audio_dec_out_frame_t out_frame{};
    out_frame.buffer = frame_buf;
    out_frame.len = frame_capacity;

    while (raw.len) {
        raw.consumed = 0;
        out_frame.decoded_size = 0;
        out_frame.needed_size = 0;
        const auto ret = esp_audio_dec_process(dec, &raw, &out_frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            auto* new_buf = static_cast<std::uint8_t*>(
                std::realloc(frame_buf, out_frame.needed_size));
            if (!new_buf) {
                ESP_LOGE(kTag, "realloc(%u) failed",
                         static_cast<unsigned>(out_frame.needed_size));
                break;
            }
            frame_buf = new_buf;
            frame_capacity = out_frame.needed_size;
            out_frame.buffer = frame_buf;
            out_frame.len = frame_capacity;
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(kTag, "esp_audio_dec_process failed: %d", static_cast<int>(ret));
            break;
        }
        if (out_frame.decoded_size > 0) {
            const auto* src16 = reinterpret_cast<const std::int16_t*>(frame_buf);
            pcm.insert(pcm.end(), src16,
                       src16 + out_frame.decoded_size / sizeof(std::int16_t));
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        if (raw.consumed == 0) {
            // Avoid spinning on bad input.
            break;
        }
    }
    std::free(frame_buf);

    esp_audio_dec_info_t info{};
    if (esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
        ESP_LOGI(kTag, "decoded %u PCM samples (sr=%u ch=%u)",
                 static_cast<unsigned>(pcm.size()),
                 static_cast<unsigned>(info.sample_rate),
                 static_cast<unsigned>(info.channel));
    }
    esp_audio_dec_close(dec);

    if (pcm.empty()) {
        return false;
    }

    M5.Speaker.playRaw(pcm.data(), pcm.size(), recording.sample_rate, /*stereo=*/false);
    while (M5.Speaker.isPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

} // namespace stackchan::app
