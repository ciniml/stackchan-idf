// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HMM (hts_engine) 合成エンジン。flash mmap / PSRAM 上の .htsvoice イメージを
// ロードし、かな→full-context ラベル (hts_label.cpp) で合成する。
// 48 kHz ボイスでも出力レート (16 kHz) でボコーダを直接回す (α 再設定)。
//
// CONFIG_JTTS_ENABLE_HMM が無効なボード (flash に voice を置けない 8 MB 構成)
// ではスタブになり、hts_engine のコードはリンクされない。
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"

#if !defined(ESP_PLATFORM) || defined(CONFIG_JTTS_ENABLE_HMM)
#define JTTS_HMM_AVAILABLE 1
#endif

#ifdef JTTS_HMM_AVAILABLE

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include "HTS_engine.h"

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#endif

namespace stackchan::jtts {

namespace {
HTS_Engine g_engine;
bool g_loaded = false;
// エンジンは単一のミュータブル構造なので、合成中の差し替え (HTTP アップロード
// タスク vs 発話タスク) を直列化する。合成は数百 ms 保持するが、差し替えは
// 稀なので単純なミューテックスで足りる。
std::mutex g_engine_mutex;
}  // namespace

bool set_hmm_voice(std::span<const std::uint8_t> htsvoice) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_loaded) {
        HTS_Engine_clear(&g_engine);
        g_loaded = false;
    }
    if (htsvoice.empty()) return true;
    HTS_Engine_initialize(&g_engine);
    const void* datas[1] = {htsvoice.data()};
    const size_t sizes[1] = {htsvoice.size()};
    if (HTS_Engine_load_data(&g_engine, datas, sizes, 1) != TRUE) {
        HTS_Engine_clear(&g_engine);
        return false;
    }
    g_loaded = true;
    return true;
}

bool hmm_voice_loaded() {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    return g_loaded;
}

namespace internal {

bool render_hmm(std::u32string_view text, std::vector<std::int16_t>& out, const Options& opt) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (!g_loaded) return false;

    // ボイスのネイティブ レート (48 kHz) と出力レート (16 kHz) が異なる場合は
    // ボコーダを出力レートで直接回す (α をメル尺度に合わせて再設定)。
    // デシメーションより 3 倍速く、スペクトル差は ~0.8 dB (ホスト検証)。
    const std::size_t voice_rate = g_engine.ms.sampling_frequency;
    const std::size_t voice_fperiod = g_engine.ms.frame_period;
    float alpha;
    if (opt.sample_rate_hz == voice_rate) {
        alpha = g_engine.condition.alpha;
    } else if (voice_rate == 48000 && opt.sample_rate_hz == 16000) {
        alpha = 0.42f;  // 16 kHz のメル尺度近似 (HTS 慣例値)
    } else {
        return false;  // 対応外レート → フォールバック
    }
    HTS_Engine_set_sampling_frequency(&g_engine, opt.sample_rate_hz);
    HTS_Engine_set_fperiod(&g_engine, voice_fperiod * opt.sample_rate_hz / voice_rate);
    HTS_Engine_set_alpha(&g_engine, alpha);

    std::vector<std::string> labels;
    if (!build_hts_labels(text, labels)) return false;

    std::vector<char*> lines;
    lines.reserve(labels.size());
    for (auto& l : labels) lines.push_back(l.data());

    // mora_ms は「1 モーラの長さ」なので speed は逆比。既定 110 ms = 等速。
    float speed = 110.0f / opt.mora_ms;
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 2.0f) speed = 2.0f;
    HTS_Engine_set_speed(&g_engine, speed);
    HTS_Engine_add_half_tone(&g_engine, opt.hmm_half_tone);

    const auto t0 = std::chrono::steady_clock::now();
    if (HTS_Engine_synthesize_from_strings(&g_engine, lines.data(), lines.size()) != TRUE) {
        HTS_Engine_refresh(&g_engine);
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();

    const std::size_t nsamples = HTS_Engine_get_nsamples(&g_engine);
    const float* speech = g_engine.gss.gspeech;  // per-sample getter は高いので直接参照
    const float gain = opt.gain;
    out.reserve(out.size() + nsamples);
    for (std::size_t i = 0; i < nsamples; ++i) {
        float v = speech[i] * gain;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out.push_back(static_cast<std::int16_t>(v));
    }
    const auto t2 = std::chrono::steady_clock::now();

#if defined(ESP_PLATFORM)
    ESP_LOGI("jtts-hmm", "synth %u ms + copy %u ms for %u samples @%u Hz",
             static_cast<unsigned>(
                 std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()),
             static_cast<unsigned>(
                 std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()),
             static_cast<unsigned>(nsamples), static_cast<unsigned>(voice_rate));
#endif

    HTS_Engine_refresh(&g_engine);
    return true;
}

}  // namespace internal
}  // namespace stackchan::jtts

#else  // !JTTS_HMM_AVAILABLE — スタブ (hts_engine をリンクしない)

namespace stackchan::jtts {

bool set_hmm_voice(std::span<const std::uint8_t>) { return false; }
bool hmm_voice_loaded() { return false; }

namespace internal {
bool render_hmm(std::u32string_view, std::vector<std::int16_t>&, const Options&) { return false; }
}  // namespace internal

}  // namespace stackchan::jtts

#endif  // JTTS_HMM_AVAILABLE
