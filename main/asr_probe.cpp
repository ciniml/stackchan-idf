// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// [ASR Phase 0] esp-sr AFE + WakeNet を実機で起動し、(1) 内部 RAM の消費/残量を
// 実測、(2) 日本語ウェイクワード「こんにちは ESP」の検出を確認する。cores3 +
// camera off 前提。go/no-go はここの heap ログで決まる。
#include "asr_probe.hpp"

#include "sdkconfig.h"

#if defined(CONFIG_STACKCHAN_ASR_ENABLED)

#include <atomic>
#include <cstdint>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>
#include <esp_timer.h>

#include "shared_state.hpp"
#include "avatar/expression.hpp"
#include "settings_sinks.hpp"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"  // ESP_WN_PREFIX
#include "model_path.h"

namespace {

constexpr const char* kTag = "asr";

const esp_afe_sr_iface_t* g_afe = nullptr;
esp_afe_sr_data_t* g_afe_data = nullptr;
stackchan::app::SharedState* g_state = nullptr;

// 返事の発話中フラグ。マイクとスピーカーは I2S 排他なので、ウェイクワード検出時に
// これを立てて feed_task にマイクを手放させ、say_worker のスピーカー再生が I2S を
// 取れるようにする。発話終了後に feed_task がマイクを取り戻す。
std::atomic<bool> g_speaking{false};

void log_heap(const char* when) {
    ESP_LOGI(kTag, "heap[%s]: INT free=%u largest=%u | DMA largest=%u | PSRAM free=%u", when,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

// マイク供給タスク: 16kHz mono を chunk 単位で AFE に feed。発話中は休止。
void feed_task(void*) {
    const int chunk = g_afe->get_feed_chunksize(g_afe_data);
    const int ch = g_afe->get_channel_num(g_afe_data);
    auto* buf = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * chunk * ch, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        ESP_LOGE(kTag, "feed buf alloc failed (%d samples)", chunk * ch);
        vTaskDelete(nullptr);
        return;
    }
    // mic_lip_sync_task と同じ排他手順: Speaker.end() → I2S settle 待ち → Mic.begin()。
    // settle 待ちを挟まないと I2S ドライバが未アンインストールで "controller occupied"。
    constexpr TickType_t kI2sSettle = pdMS_TO_TICKS(20);
    bool mic_owned = false;
    bool spk_played = false;
    int64_t speak_since = 0;
    for (;;) {
        // 発話要求 (g_speaking) or 再生中はマイクを手放して I2S をスピーカーに渡す。
        const bool speaking = g_speaking.load(std::memory_order_relaxed);
        if (speaking || M5.Speaker.isPlaying()) {
            if (mic_owned) {
                M5.Mic.end();
                vTaskDelay(kI2sSettle);
                mic_owned = false;
            }
            if (M5.Speaker.isPlaying()) spk_played = true;
            if (speaking) {
                const int64_t now = esp_timer_get_time() / 1000;
                if (speak_since == 0) speak_since = now;
                // 一度再生されて止まった or タイムアウト(合成遅延+再生)で発話終了とみなす
                if ((spk_played && !M5.Speaker.isPlaying()) || (now - speak_since > 6000)) {
                    g_speaking.store(false, std::memory_order_relaxed);
                    spk_played = false;
                    speak_since = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!mic_owned) {
            M5.Speaker.end();
            vTaskDelay(kI2sSettle);
            if (!M5.Mic.begin()) {
                ESP_LOGW(kTag, "M5.Mic.begin failed; retry in 1s");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            mic_owned = true;
            ESP_LOGI(kTag, "mic acquired; feeding AFE");
        }
        // AFE は 1ch × feed_chunksize サンプルを期待。
        if (!M5.Mic.record(buf, chunk, 16000, false)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (int i = 0; i < 100 && M5.Mic.isRecording(); ++i) vTaskDelay(pdMS_TO_TICKS(2));
        // マイク生振幅の確認 (throttle)
        static int fc = 0; static int amax = 0;
        for (int i = 0; i < chunk; ++i) { int a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > amax) amax = a; }
        if (++fc % 50 == 0) { ESP_LOGI(kTag, "mic raw |max|=%d", amax); amax = 0; }
        g_afe->feed(g_afe_data, buf);
    }
}

// 検出タスク: AFE から fetch してウェイクワードを見る。
void detect_task(void*) {
    int cnt = 0;
    float vmax = -999.0f;
    for (;;) {
        afe_fetch_result_t* r = g_afe->fetch(g_afe_data);
        if (r == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (r->data_volume > vmax) vmax = r->data_volume;
        if (++cnt % 30 == 0) {  // マイクが音を拾えているか可視化 (無音≈-70dB, 発話で上昇)
            ESP_LOGI(kTag, "listening... vol max=%.1fdB", static_cast<double>(vmax));
            vmax = -999.0f;
        }
        if (r->wakeup_state == WAKENET_DETECTED) {
            static int64_t last_ms = 0;
            const int64_t now = esp_timer_get_time() / 1000;
            if (now - last_ms < 1500) continue;  // debounce (連続発火抑制)
            last_ms = now;
            // 発話前にマイクを手放させる (I2S をスピーカーに渡す)。say_worker は先に
            // 合成 (~1s) してから playRaw するので、その間に feed_task が解放する。
            g_speaking.store(true, std::memory_order_relaxed);
            stackchan::app::settings_sinks::say_kana("はい");  // 声で「はい」と返事
            ESP_LOGI(kTag, "★★ WAKE WORD DETECTED (idx=%d) ★★", r->wake_word_index);
            if (g_state != nullptr) {
                g_state->face.expression.store(static_cast<int>(stackchan::avatar::Expression::Happy),
                                               std::memory_order_relaxed);
                g_state->set_balloon_text("はいï¼", 1800);  // 「はい？」
            }
        }
    }
}

}  // namespace

void asr_probe_run(stackchan::app::SharedState& state) {
    g_state = &state;
    log_heap("before esp-sr");

    srmodel_list_t* models = esp_srmodel_init("model");
    if (models == nullptr || models->num <= 0) {
        ESP_LOGE(kTag, "esp_srmodel_init(\"model\") found no models (partition empty?)");
        return;
    }
    char* wn = esp_srmodel_filter(models, ESP_WN_PREFIX, nullptr);
    ESP_LOGI(kTag, "models=%d, wakenet=%s", models->num, wn ? wn : "(none)");
    if (wn == nullptr) {
        ESP_LOGE(kTag, "no wakenet model in partition");
        return;
    }

    afe_config_t* cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (cfg == nullptr) {
        ESP_LOGE(kTag, "afe_config_init failed");
        return;
    }
    cfg->aec_init = false;   // スピーカーと I2S 排他共有 → 自己エコー除去は使わない
    cfg->wakenet_mode = DET_MODE_90;  // 高感度 (Normal)
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;  // 内部RAM節約 (PSRAM 寄せ)
    cfg->pcm_config.total_ch_num = 1;  // mono mic のみ
    cfg->pcm_config.mic_num = 1;
    cfg->pcm_config.ref_num = 0;

    g_afe = esp_afe_handle_from_config(cfg);
    g_afe_data = g_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (g_afe_data == nullptr) {
        ESP_LOGE(kTag, "AFE create_from_config failed");
        return;
    }

    log_heap("after AFE create");
    ESP_LOGI(kTag, "AFE: feed_chunk=%d ch=%d samp_rate=%d", g_afe->get_feed_chunksize(g_afe_data),
             g_afe->get_channel_num(g_afe_data), g_afe->get_samp_rate(g_afe_data));

    // タスク スタックは PSRAM に置き、内部RAMは BLE/httpd/WiFi の通信系に温存する。
    // feed/detect は flash 操作をしない (WakeNet モデルは PSRAM) ので PSRAM スタックで
    // 安全。TCB は内部RAM (BSS)。
    static StaticTask_t s_feed_tcb, s_det_tcb;
    constexpr int kFeedStack = 4096, kDetStack = 6144;
    auto* feed_stk = static_cast<StackType_t*>(heap_caps_malloc(kFeedStack, MALLOC_CAP_SPIRAM));
    auto* det_stk = static_cast<StackType_t*>(heap_caps_malloc(kDetStack, MALLOC_CAP_SPIRAM));
    if (feed_stk != nullptr && det_stk != nullptr) {
        xTaskCreateStaticPinnedToCore(feed_task, "asr_feed", kFeedStack, nullptr, 5, feed_stk,
                                      &s_feed_tcb, 0);
        xTaskCreateStaticPinnedToCore(detect_task, "asr_det", kDetStack, nullptr, 5, det_stk,
                                      &s_det_tcb, 1);
    } else {  // PSRAM 確保失敗時は内部RAMにフォールバック
        xTaskCreatePinnedToCore(feed_task, "asr_feed", kFeedStack, nullptr, 5, nullptr, 0);
        xTaskCreatePinnedToCore(detect_task, "asr_det", kDetStack, nullptr, 5, nullptr, 1);
    }
    ESP_LOGI(kTag, "ASR probe running — say「こんにちは ESP」");
}

#else   // !CONFIG_STACKCHAN_ASR_ENABLED

#include "shared_state.hpp"
void asr_probe_run(stackchan::app::SharedState&) {}

#endif
