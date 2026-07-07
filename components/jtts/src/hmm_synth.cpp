// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HMM (hts_engine) 合成エンジン。flash mmap / PSRAM 上の .htsvoice イメージを
// ロードし、かな→full-context ラベル (hts_label.cpp) で合成する。
// 48 kHz ボイスはネイティブ合成 → FIR 1/3 デシメーションで 16 kHz 化。
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

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>

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

namespace {

// 48 kHz → 16 kHz 用 1/3 デシメータ (45-tap Hamming 窓 sinc、fc = 7.2 kHz)
constexpr std::size_t kDecimTaps = 45;

const std::array<float, kDecimTaps>& decim_coeffs() {
    static const std::array<float, kDecimTaps> coeffs = [] {
        std::array<float, kDecimTaps> h{};
        constexpr float fc = 7200.0f / 48000.0f;  // 正規化カットオフ
        constexpr int mid = static_cast<int>(kDecimTaps) / 2;
        float sum = 0.0f;
        for (int i = 0; i < static_cast<int>(kDecimTaps); ++i) {
            const int k = i - mid;
            const float x = 2.0f * std::numbers::pi_v<float> * fc;
            const float sinc = (k == 0) ? 2.0f * fc : std::sin(x * k) / (std::numbers::pi_v<float> * k);
            const float w = 0.54f - 0.46f * std::cos(2.0f * std::numbers::pi_v<float> * i / (kDecimTaps - 1));
            h[i] = sinc * w;
            sum += h[i];
        }
        for (auto& v : h) v /= sum;  // DC ゲイン 1
        return h;
    }();
    return coeffs;
}

}  // namespace

bool render_hmm(std::u32string_view text, std::vector<std::int16_t>& out, const Options& opt) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (!g_loaded) return false;

    // ボイスはネイティブ レート (48 kHz) のまま合成し、出力レート (16 kHz) へは
    // FIR 1/3 デシメーションで落とす。ボコーダを 16 kHz で直接回す (α 再設定)
    // 近似も試したが、メルケプの周波数軸はどの α でも 48 kHz 分析軸と一致せず
    // フォルマントが下方に歪む (声が暗く低く聞こえる) ため不採用。
    const std::size_t voice_rate = g_engine.ms.sampling_frequency;
    std::size_t decim;
    if (opt.sample_rate_hz == voice_rate) {
        decim = 1;
    } else if (voice_rate == 3 * opt.sample_rate_hz) {
        decim = 3;
    } else {
        return false;  // 対応外レート → フォールバック
    }

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
    if (decim == 1) {
        out.reserve(out.size() + nsamples);
        for (std::size_t i = 0; i < nsamples; ++i) {
            float v = speech[i] * gain;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out.push_back(static_cast<std::int16_t>(v));
        }
    } else {
        // 1/3 ポリフェーズ デシメーション (45-tap Hamming sinc、fc=7.2 kHz)。
        // 出力サンプルあたり実質 15 MAC なので合成コストに対して無視できる。
        const auto& h = decim_coeffs();
        constexpr int mid = static_cast<int>(kDecimTaps) / 2;
        const std::size_t nout = nsamples / 3;
        out.reserve(out.size() + nout);
        for (std::size_t n = 0; n < nout; ++n) {
            const long center = static_cast<long>(n) * 3;
            long lo = center - mid;
            long hi = center + mid;  // inclusive
            int skip = 0;
            if (lo < 0) {
                skip = static_cast<int>(-lo);
                lo = 0;
            }
            if (hi >= static_cast<long>(nsamples)) hi = static_cast<long>(nsamples) - 1;
            float acc = 0.0f;
            const float* hp = h.data() + skip;
            const float* sp = speech + lo;
            for (long i = lo; i <= hi; ++i) acc += *hp++ * *sp++;
            acc *= gain;
            if (acc > 32767.0f) acc = 32767.0f;
            if (acc < -32768.0f) acc = -32768.0f;
            out.push_back(static_cast<std::int16_t>(acc));
        }
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
