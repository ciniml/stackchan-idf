// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 定 peak-gain バンドパス biquad。無声 (摩擦) 成分の並列パス用。
// 並列構成はフォルマントごとの振幅 (a1..a3) を直接制御できるので、
// 子音テーブルのチューニング (「した」vs「ちた」問題など) をそのまま活かす。
class Biquad {
public:
    void set_bpf(float f0, float bw, float fs) {
        if (f0 < 50.0f) f0 = 50.0f;
        if (f0 > fs * 0.45f) f0 = fs * 0.45f;
        if (bw < 30.0f) bw = 30.0f;
        if (bw > f0 * 2.0f) bw = f0 * 2.0f;
        float w0 = 2.0f * kPi * f0 / fs;
        float Q = f0 / bw;
        float alpha = std::sin(w0) / (2.0f * Q);
        float cos_w0 = std::cos(w0);
        float a0 = 1.0f + alpha;
        b0_ = alpha / a0;
        b1_ = 0.0f;
        b2_ = -alpha / a0;
        a1_ = -2.0f * cos_w0 / a0;
        a2_ = (1.0f - alpha) / a0;
    }
    float process(float x) {
        float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_;
        x1_ = x;
        y2_ = y1_;
        y1_ = y;
        return y;
    }
    void reset() { x1_ = x2_ = y1_ = y2_ = 0.0f; }

private:
    float b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

// Klatt 型 2-pole 共振器 (DC ゲイン 1)。有声パスのカスケード接続用。
// カスケードにするとフォルマント間の相対振幅が声道の物理と同じ形で自動的に
// 決まり、並列構成で起きるフォルマント間の位相打ち消しの谷が出ないため、
// 母音の了解性が上がる (Klatt 1980 のカスケード分岐と同じ考え方)。
class Resonator {
public:
    void set(float f, float bw, float fs) {
        if (f < 50.0f) f = 50.0f;
        if (f > fs * 0.47f) f = fs * 0.47f;
        if (bw < 30.0f) bw = 30.0f;
        const float T = 1.0f / fs;
        c_ = -std::exp(-2.0f * kPi * bw * T);
        b_ = 2.0f * std::exp(-kPi * bw * T) * std::cos(2.0f * kPi * f * T);
        a_ = 1.0f - b_ - c_;
    }
    float process(float x) {
        float y = a_ * x + b_ * y1_ + c_ * y2_;
        y2_ = y1_;
        y1_ = y;
        return y;
    }
    void reset() { y1_ = y2_ = 0.0f; }

private:
    float a_ = 0, b_ = 0, c_ = 0;
    float y1_ = 0, y2_ = 0;
};

// Rosenberg 声門流の微分 (glottal flow derivative) を励起源にする。
// 旧実装のインパルス列は全帯域フラットなスペクトルでブザー的な耳障りさの
// 主因だった。声門流微分は開大期のゆるい山 + 閉鎖時の鋭い負スパイクという
// 形をしており、自然な -6 dB/oct 程度のスペクトル傾斜 (放射特性込み) が
// 得られる。フォルマント合成の「機械のブザー」感がここで一番減る。
//   flow g(ph):  0 ≤ ph < Tp : 0.5 (1 - cos(π ph / Tp))       (開大)
//                Tp ≤ ph < Tc : cos(π (ph - Tp) / (2 Tn))      (閉小)
//                Tc ≤ ph < 1  : 0                              (閉鎖)
//   励起は dg/dph。Tp = 0.40, Tn = 0.16 (open quotient 0.56)。
class GlottalSource {
public:
    float tick(float f0_hz, float fs) {
        if (f0_hz <= 1.0f) return 0.0f;
        phase_ += f0_hz / fs;
        if (phase_ >= 1.0f) phase_ -= 1.0f;

        constexpr float kTp = 0.40f;
        constexpr float kTn = 0.16f;
        constexpr float kTc = kTp + kTn;
        if (phase_ < kTp) {
            return (kPi / (2.0f * kTp)) * std::sin(kPi * phase_ / kTp);
        }
        if (phase_ < kTc) {
            return -(kPi / (2.0f * kTn)) *
                   std::sin(kPi * (phase_ - kTp) / (2.0f * kTn));
        }
        return 0.0f;
    }
    void reset() { phase_ = 0.0f; }

private:
    float phase_ = 0.0f;
};

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline std::int16_t to_i16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<std::int16_t>(x * 32760.0f);
}

}  // namespace

void render_segments(std::span<const Segment> segs, std::vector<std::int16_t>& out,
                     const Options& opt) {
    const float fs = static_cast<float>(opt.sample_rate_hz);

    // 有声パス: 声門波 → R1→R2→R3→R4→R5 カスケード。
    Resonator c1, c2, c3, c4, c5;
    // 無声パス: ノイズ → 並列 BPF ×3 (従来どおり、a1..a3 で振幅制御)。
    Biquad p1, p2, p3;
    GlottalSource voice;
    std::mt19937 rng(0xC0DECAFEu);
    auto noise = [&]() {
        std::uint32_t v = rng();
        return (static_cast<float>(v) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
    };

    // F4/F5 は母音間でほとんど動かないので固定共振器として置く。3 フォルマント
    // では 3 kHz 以上がごっそり欠けて「こもった電話声」になるのを埋める。
    // 声道長の違い (formant_scale) には追従させる。
    const float fscale = (opt.formant_scale > 0.0f) ? opt.formant_scale : 1.0f;
    c4.set(3300.0f * fscale, 280.0f, fs);
    c5.set(3850.0f * fscale, 320.0f, fs);

    constexpr float step_ms = 5.0f;
    const std::size_t step_samples =
        std::max<std::size_t>(1, static_cast<std::size_t>(step_ms * 0.001f * fs));

    const bool vibrato_on = opt.vibrato_rate_hz > 0.0f && opt.vibrato_cents > 0.0f;
    const float vib_inc = opt.vibrato_rate_hz / fs;
    float vib_phase = 0.0f;
    // ノイズ (一様乱数 [-1,1]、RMS = 1/√3) を声門波の実効振幅と揃えるための係数。
    constexpr float kNoiseGain = 1.73205081f;
    // カスケード出力の全体ゲイン。声門波振幅 (≈π/2Tn) とカスケードの帯域圧縮を
    // 込みで、旧実装 (並列 + インパルス) と同程度の出力 RMS になるよう
    // ホストの母音テスト (test_vowels) で合わせた値。
    constexpr float kVoicedMakeup = 0.015f;

    for (const Segment& seg : segs) {
        std::size_t total = static_cast<std::size_t>(std::lround(seg.duration_ms * 0.001f * fs));
        if (total == 0) continue;

        std::size_t k = 0;
        while (k < total) {
            std::size_t n = std::min(step_samples, total - k);
            float t = static_cast<float>(k + n / 2) / static_cast<float>(total);

            float f1 = lerp(seg.start.f1, seg.end.f1, t);
            float f2 = lerp(seg.start.f2, seg.end.f2, t);
            float f3 = lerp(seg.start.f3, seg.end.f3, t);
            float bw1 = lerp(seg.start.bw1, seg.end.bw1, t);
            float bw2 = lerp(seg.start.bw2, seg.end.bw2, t);
            float bw3 = lerp(seg.start.bw3, seg.end.bw3, t);
            float a1 = lerp(seg.start.a1, seg.end.a1, t);
            float a2 = lerp(seg.start.a2, seg.end.a2, t);
            float a3 = lerp(seg.start.a3, seg.end.a3, t);
            float voicing = lerp(seg.start.voicing, seg.end.voicing, t);
            float frication = lerp(seg.start.frication, seg.end.frication, t);
            float f0_base = lerp(seg.start.f0_hz, seg.end.f0_hz, t);

            c1.set(f1, bw1, fs);
            c2.set(f2, bw2, fs);
            c3.set(f3, bw3, fs);
            p1.set_bpf(f1, bw1, fs);
            p2.set_bpf(f2, bw2, fs);
            p3.set_bpf(f3, bw3, fs);

            // カスケードは相対振幅を構造が決めるので、フォルマント別振幅の
            // 代わりに a1 を「その区間の有声マスター音量」として使う (母音 1.0、
            // 鼻音 0.55、無声化母音 0.55 などテーブルの意図はそのまま活きる)。
            const float voiced_amp = a1;

            for (std::size_t j = 0; j < n; ++j) {
                float f0_mod = f0_base;
                if (vibrato_on) {
                    vib_phase += vib_inc;
                    if (vib_phase >= 1.0f) vib_phase -= 1.0f;
                    float lfo = std::sin(2.0f * kPi * vib_phase);
                    f0_mod = f0_base * std::exp2(opt.vibrato_cents / 1200.0f * lfo);
                }

                float pulse = voice.tick(f0_mod, fs);
                float n1 = noise() * kNoiseGain;
                float n2 = noise() * kNoiseGain;
                float voiced_src = (1.0f - opt.breathiness) * pulse + opt.breathiness * n1;

                float voiced =
                    c5.process(c4.process(c3.process(c2.process(c1.process(voiced_src)))));
                voiced *= voicing * voiced_amp * opt.voicing_mul * kVoicedMakeup;

                float fric_src = frication * n2 * opt.frication_mul;
                float fric = a1 * p1.process(fric_src) + a2 * p2.process(fric_src) +
                             a3 * p3.process(fric_src);

                float y = (voiced + fric) * opt.gain;
                out.push_back(to_i16(y));
            }
            k += n;
        }
    }
}

}  // namespace stackchan::jtts::internal
