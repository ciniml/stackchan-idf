// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// フォルマント合成 (V2 / Classic 両バリアント) が共有する DSP 部品。
#pragma once

#include <cmath>
#include <cstdint>

namespace stackchan::jtts::internal::dsp {

constexpr float kPi = 3.14159265358979323846f;

// 定 peak-gain バンドパス biquad。並列フォルマント パス用 (Classic の全パス、
// V2 の摩擦パス)。並列構成はフォルマントごとの振幅 (a1..a3) を直接制御できる
// ので、子音テーブルのチューニングをそのまま活かせる。
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

// Klatt 型 2-pole 共振器 (DC ゲイン 1)。V2 の有声カスケード用。
// カスケードにするとフォルマント間の相対振幅が声道の物理と同じ形で自動的に
// 決まり、並列構成で起きるフォルマント間の位相打ち消しの谷が出ない。
//
// 係数はブロック境界の階段ではなく per-sample の線形補間で更新する
// (set_target + ランプ)。5 ms ごとの係数ステップは微小な広帯域過渡を出し、
// それが -18 dB 程度の HF フロアになって tilt_db の効きを覆い隠すのを
// 実測で確認したため (隣接ブロックの係数差は小さいので補間中の安定性は
// 実用上問題ない)。
class Resonator {
public:
    // 即時設定 (固定共振器 F4/F5 や初期化用)。
    void set(float f, float bw, float fs) {
        compute(f, bw, fs, a_, b_, c_);
        ramp_left_ = 0;
    }
    // ramp_samples かけて目標係数へ線形補間する。
    void set_target(float f, float bw, float fs, int ramp_samples) {
        float ta, tb, tc;
        compute(f, bw, fs, ta, tb, tc);
        if (ramp_samples <= 1) {
            a_ = ta;
            b_ = tb;
            c_ = tc;
            ramp_left_ = 0;
            return;
        }
        const float inv = 1.0f / static_cast<float>(ramp_samples);
        da_ = (ta - a_) * inv;
        db_ = (tb - b_) * inv;
        dc_ = (tc - c_) * inv;
        ramp_left_ = ramp_samples;
    }
    float process(float x) {
        if (ramp_left_ > 0) {
            a_ += da_;
            b_ += db_;
            c_ += dc_;
            --ramp_left_;
        }
        float y = a_ * x + b_ * y1_ + c_ * y2_;
        y2_ = y1_;
        y1_ = y;
        return y;
    }
    void reset() { y1_ = y2_ = 0.0f; }

private:
    static void compute(float f, float bw, float fs, float& a, float& b, float& c) {
        if (f < 50.0f) f = 50.0f;
        if (f > fs * 0.47f) f = fs * 0.47f;
        if (bw < 30.0f) bw = 30.0f;
        const float T = 1.0f / fs;
        c = -std::exp(-2.0f * kPi * bw * T);
        b = 2.0f * std::exp(-kPi * bw * T) * std::cos(2.0f * kPi * f * T);
        a = 1.0f - b - c;
    }
    float a_ = 0, b_ = 0, c_ = 0;
    float da_ = 0, db_ = 0, dc_ = 0;
    int ramp_left_ = 0;
    float y1_ = 0, y2_ = 0;
};

// Klatt 型 anti-resonator (2-zero、DC ゲイン 1)。Resonator の厳密な逆特性:
//   Resonator     H(z) = a / (1 - b z^-1 - c z^-2)
//   AntiResonator H(z) = (1/a) (1 - b z^-1 - c z^-2)
// なので同じ f/bw を与えた Resonator と直列にすると恒等になる (鼻音ゼロを
// 「打ち消し位置」に置いたとき V2 カスケードが完全に透過になる — 母音の
// フォルマントを一切乱さない)。係数は Resonator と同じく per-sample の
// 線形補間 (set_target) で更新し、ゼロ周波数の移動でクリックを出さない。
class AntiResonator {
public:
    // 即時設定 (初期化用)。
    void set(float f, float bw, float fs) {
        compute(f, bw, fs, a_, b_, c_);
        ramp_left_ = 0;
    }
    // ramp_samples かけて目標係数へ線形補間する。
    void set_target(float f, float bw, float fs, int ramp_samples) {
        float ta, tb, tc;
        compute(f, bw, fs, ta, tb, tc);
        if (ramp_samples <= 1) {
            a_ = ta;
            b_ = tb;
            c_ = tc;
            ramp_left_ = 0;
            return;
        }
        const float inv = 1.0f / static_cast<float>(ramp_samples);
        da_ = (ta - a_) * inv;
        db_ = (tb - b_) * inv;
        dc_ = (tc - c_) * inv;
        ramp_left_ = ramp_samples;
    }
    float process(float x) {
        if (ramp_left_ > 0) {
            a_ += da_;
            b_ += db_;
            c_ += dc_;
            --ramp_left_;
        }
        float y = a_ * x + b_ * x1_ + c_ * x2_;
        x2_ = x1_;
        x1_ = x;
        return y;
    }
    void reset() { x1_ = x2_ = 0.0f; }

private:
    static void compute(float f, float bw, float fs, float& a, float& b, float& c) {
        // Resonator::compute と同じ極公式から a' = 1/a, b' = -b/a, c' = -c/a。
        if (f < 50.0f) f = 50.0f;
        if (f > fs * 0.47f) f = fs * 0.47f;
        if (bw < 30.0f) bw = 30.0f;
        const float T = 1.0f / fs;
        const float rc = -std::exp(-2.0f * kPi * bw * T);
        const float rb = 2.0f * std::exp(-kPi * bw * T) * std::cos(2.0f * kPi * f * T);
        const float ra = 1.0f - rb - rc;
        a = 1.0f / ra;
        b = -rb / ra;
        c = -rc / ra;
    }
    float a_ = 1.0f, b_ = 0, c_ = 0;
    float da_ = 0, db_ = 0, dc_ = 0;
    int ramp_left_ = 0;
    float x1_ = 0, x2_ = 0;
};

// 1-pole ハイパス (y = a (y1 + x - x1))。歯擦音の摩擦ノイズから低域を落とす
// 用途 (母音フォルマント BPF ×3 だけでは /s/ の 4 kHz 以上のエネルギー集中を
// 作れないため)。
class HighPass1 {
public:
    void set(float fc, float fs) {
        if (fc < 10.0f) fc = 10.0f;
        if (fc > fs * 0.45f) fc = fs * 0.45f;
        a_ = std::exp(-2.0f * kPi * fc / fs);
    }
    float process(float x) {
        y1_ = a_ * (y1_ + x - x1_);
        x1_ = x;
        return y1_;
    }
    void reset() { x1_ = y1_ = 0.0f; }

private:
    float a_ = 0.0f;
    float x1_ = 0, y1_ = 0;
};

// Rosenberg 声門流の微分 (V2 の励起源)。開大期のゆるい山 + 閉鎖時の鋭い
// 負スパイクという形で、自然な -6 dB/oct 程度のスペクトル傾斜が得られる。
//   flow g(ph):  0 ≤ ph < Tp : 0.5 (1 - cos(π ph / Tp))       (開大)
//                Tp ≤ ph < Tc : cos(π (ph - Tp) / (2 Tn))      (閉小)
//                Tc ≤ ph < 1  : 0                              (閉鎖)
// open quotient (OQ = Tp + Tn) は可変: OQ を上げると閉鎖スパイク (振幅
// π/2Tn) が鈍って高域が減り、やわらかい発声になる。Tp:Tn = 2.5:1 を保つ。
// 音量の較正 (kVoicedMakeup) は崩れない: 声門流のピーク振幅は OQ に
// よらず 1 で、微分の低域成分 (= F1 帯の駆動 = 知覚的な音量の主成分) は
// flow の振れ幅で決まるため OQ 非依存。変わるのは高域だけ。
class GlottalSource {
public:
    void set_oq(float oq) {
        if (oq < 0.30f) oq = 0.30f;
        if (oq > 0.90f) oq = 0.90f;
        tp_ = oq * (2.5f / 3.5f);
        tn_ = oq * (1.0f / 3.5f);
    }
    float tick(float f0_hz, float fs) {
        if (f0_hz <= 1.0f) return 0.0f;
        phase_ += f0_hz / fs;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        if (phase_ < tp_) {
            return (kPi / (2.0f * tp_)) * std::sin(kPi * phase_ / tp_);
        }
        const float tc = tp_ + tn_;
        if (phase_ < tc) {
            return -(kPi / (2.0f * tn_)) *
                   std::sin(kPi * (phase_ - tp_) / (2.0f * tn_));
        }
        return 0.0f;
    }
    // 現在位相での声門流 (0..1)。有声摩擦音 /z/ /j/ のノイズ振幅変調用
    // (声門が開いている間だけ乱流が出る、という物理の安価な模倣)。
    float openness() const {
        if (phase_ < tp_) {
            return 0.5f * (1.0f - std::cos(kPi * phase_ / tp_));
        }
        const float tc = tp_ + tn_;
        if (phase_ < tc) {
            return std::cos(kPi * (phase_ - tp_) / (2.0f * tn_));
        }
        return 0.0f;
    }
    void reset() { phase_ = 0.0f; }

private:
    float phase_ = 0.0f;
    float tp_ = 0.40f;
    float tn_ = 0.16f;
};

// f0 の周期ごとに高さ ≈ sqrt(Fs/f0) のインパルスを出す励起源 (Classic 用)。
// BPF resonator (peak gain ≈ 0 dB) を通したときの出力 RMS が f0 に対して
// だいたい一定になるよう正規化している。
class ImpulseSource {
public:
    float tick(float f0_hz, float fs) {
        if (f0_hz <= 1.0f) return 0.0f;
        float inc = f0_hz / fs;
        phase_ += inc;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            return std::sqrt(fs / f0_hz);
        }
        return 0.0f;
    }
    void reset() { phase_ = 0.0f; }

private:
    float phase_ = 0.0f;
};

// スペクトル傾斜フィルタ: 1-pole LPF y = (1-a)x + a·y1。tilt_db は 3 kHz
// での減衰量 [dB]。Klatt 合成器の TL パラメータ相当で、やわらかさの最も
// 直接的なノブ。0 以下でバイパス。
class TiltFilter {
public:
    void set(float tilt_db, float fs) {
        if (tilt_db <= 0.01f) {
            a_ = 0.0f;
            return;
        }
        // |H(ω)| = (1-a)/√(1-2a·cosω+a²) が 3 kHz で 10^(-tilt/20) になる a
        // を解く: (1-A²)a² - 2(1-A²cosω)a + (1-A²) = 0 の小さい方の根。
        const float A = std::pow(10.0f, -tilt_db / 20.0f);
        const float A2 = A * A;
        const float w = 2.0f * kPi * 3000.0f / fs;
        const float cw = std::cos(w);
        const float p = 1.0f - A2 * cw;
        const float q = 1.0f - A2;
        float disc = p * p - q * q;
        if (disc < 0.0f) disc = 0.0f;
        a_ = (p - std::sqrt(disc)) / q;
        if (a_ < 0.0f) a_ = 0.0f;
        if (a_ > 0.995f) a_ = 0.995f;
    }
    float process(float x) {
        if (a_ <= 0.0f) return x;
        y1_ = (1.0f - a_) * x + a_ * y1_;
        return y1_;
    }
    void reset() { y1_ = 0.0f; }

private:
    float a_ = 0.0f;
    float y1_ = 0.0f;
};

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline std::int16_t to_i16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<std::int16_t>(x * 32760.0f);
}

}  // namespace stackchan::jtts::internal::dsp
