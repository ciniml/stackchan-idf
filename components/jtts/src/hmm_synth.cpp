// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HMM (hts_engine) 合成エンジン。flash mmap / PSRAM 上の .htsvoice イメージを
// ロードし、かな→full-context ラベル (hts_label.cpp) で合成する。
// ボイスは 48 kHz なので出力レート (16 kHz) へは FIR 1/3 デシメーション。
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
#include <cmath>
#include <cstring>
#include <numbers>

#include "HTS_engine.h"

namespace stackchan::jtts {

namespace {
HTS_Engine g_engine;
bool g_loaded = false;
}  // namespace

bool set_hmm_voice(std::span<const std::uint8_t> htsvoice) {
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

bool hmm_voice_loaded() { return g_loaded; }

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
    if (!g_loaded) return false;

    const std::size_t voice_rate = HTS_Engine_get_sampling_frequency(&g_engine);
    std::size_t decim = 0;
    if (voice_rate == opt.sample_rate_hz) {
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

    if (HTS_Engine_synthesize_from_strings(&g_engine, lines.data(), lines.size()) != TRUE) {
        HTS_Engine_refresh(&g_engine);
        return false;
    }

    const std::size_t nsamples = HTS_Engine_get_nsamples(&g_engine);
    const float gain = opt.gain;
    if (decim == 1) {
        out.reserve(out.size() + nsamples);
        for (std::size_t i = 0; i < nsamples; ++i) {
            float v = HTS_Engine_get_generated_speech(&g_engine, i) * gain;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out.push_back(static_cast<std::int16_t>(v));
        }
    } else {
        const auto& h = decim_coeffs();
        constexpr int mid = static_cast<int>(kDecimTaps) / 2;
        const std::size_t nout = nsamples / 3;
        out.reserve(out.size() + nout);
        for (std::size_t n = 0; n < nout; ++n) {
            const long center = static_cast<long>(n) * 3;
            float acc = 0.0f;
            for (int t = 0; t < static_cast<int>(kDecimTaps); ++t) {
                const long idx = center + t - mid;
                if (idx < 0 || idx >= static_cast<long>(nsamples)) continue;
                acc += h[static_cast<std::size_t>(t)] *
                       HTS_Engine_get_generated_speech(&g_engine, static_cast<std::size_t>(idx));
            }
            acc *= gain;
            if (acc > 32767.0f) acc = 32767.0f;
            if (acc < -32768.0f) acc = -32768.0f;
            out.push_back(static_cast<std::int16_t>(acc));
        }
    }

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
