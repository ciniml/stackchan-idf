// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// フォルマント合成 V2: Rosenberg 声門波励起 + Klatt 共振器カスケード。
// Classic バリアント (インパルス + 並列) は formant_synth_classic.cpp。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "internal.hpp"
#include "synth_dsp.hpp"

namespace stackchan::jtts::internal {

namespace {

using namespace dsp;

void render_v2(std::span<const Segment> segs, std::vector<std::int16_t>& out,
               const Options& opt) {
    const float fs = static_cast<float>(opt.sample_rate_hz);
    const float bw_scale = (opt.bw_scale > 0.0f) ? opt.bw_scale : 1.0f;

    // 有声パス: 声門波 → tilt → NP→NZ→R1→R2→R3→R4→R5 カスケード。
    // NP (鼻音極) は固定、NZ (鼻音ゼロ) は nasal=0 で極と同位置 (= 厳密に
    // 打ち消して透過)、nasal>0 で nasal_zero_hz へ移動して反共振が現れる。
    Resonator c1, c2, c3, c4, c5, np;
    AntiResonator nz;
    // 無声パス: ノイズ → 並列 BPF ×3 (a1..a3 で振幅制御)。
    // 歯擦音 (Sibilant/Palatal) は代わりに HPF + 広帯域ピークで整形する。
    Biquad p1, p2, p3;
    // fric_hp2 は歯擦音のみ直列 2 段目 (1-pole 6 dB/oct では 4 kHz カットでも
    // 1 kHz 帯の漏れが大きく /s/ の鋭さが出ないため 12 dB/oct にする)。
    HighPass1 fric_hp, fric_hp2;
    Biquad fric_peak;
    GlottalSource voice;
    voice.set_oq(opt.glottal_oq);
    TiltFilter tilt;
    tilt.set(opt.tilt_db, fs);
    std::mt19937 rng(0xC0DECAFEu);
    auto noise = [&]() {
        std::uint32_t v = rng();
        return (static_cast<float>(v) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
    };

    // F4/F5 は母音間でほとんど動かないので固定共振器として置く。3 フォルマント
    // では 3 kHz 以上がごっそり欠けて「こもった電話声」になるのを埋める。
    // 声道長の違い (formant_scale) には追従させる。
    const float fscale = (opt.formant_scale > 0.0f) ? opt.formant_scale : 1.0f;
    c4.set(3300.0f * fscale, 280.0f * bw_scale, fs);
    c5.set(3850.0f * fscale, 320.0f * bw_scale, fs);

    // 鼻音極は固定。フレーム側の nasal_zero_hz は apply_formant_scale 済み
    // なので、極も同じ fscale で追従させて打ち消し位置を揃える。
    const float nasal_pole_hz = kNasalPoleHz * fscale;
    const float nasal_bw = kNasalBwHz * bw_scale;
    np.set(nasal_pole_hz, nasal_bw, fs);
    nz.set(nasal_pole_hz, nasal_bw, fs);

    constexpr float step_ms = 5.0f;
    const std::size_t step_samples =
        std::max<std::size_t>(1, static_cast<std::size_t>(step_ms * 0.001f * fs));

    const bool vibrato_on = opt.vibrato_rate_hz > 0.0f && opt.vibrato_cents > 0.0f;
    const float vib_inc = opt.vibrato_rate_hz / fs;
    float vib_phase = 0.0f;
    // ノイズ (一様乱数 [-1,1]、RMS = 1/√3) を声門波の実効振幅と揃えるための係数。
    constexpr float kNoiseGain = 1.73205081f;
    // カスケード出力の全体ゲイン。声門波振幅 (≈π/2Tn) とカスケードの帯域圧縮を
    // 込みで、Classic (並列 + インパルス) と同程度の出力 RMS になるよう
    // ホストの母音テスト (test_vowels) で合わせた値。
    constexpr float kVoicedMakeup = 0.015f;

    // 歯擦音整形パスの較正ゲイン。従来 (母音フォルマント BPF ×3) の /s/ /sh/
    // 出力 RMS と同程度になるようホストで実測して合わせた値 (BPF は帯域幅が
    // 狭くノイズ電力の通過率が低いのに対し、HPF はほぼ半帯域を通すため)。
    constexpr float kSibilantGain = 0.45f;
    constexpr float kPalatalGain = 0.55f;
    // 6.5 kHz / 3 kHz ピークを HPF 出力に足し込む混合率。
    constexpr float kFricPeakMix = 1.0f;

    for (const Segment& seg : segs) {
        std::size_t total = static_cast<std::size_t>(std::lround(seg.duration_ms * 0.001f * fs));
        if (total == 0) continue;

        // 摩擦整形はセグメント単位 (補間しない — 調音位置は離散量)。
        const FricationShape fshape = seg.start.fric_shape;
        if (fshape == FricationShape::Sibilant) {
            fric_hp.set(4000.0f * fscale, fs);
            fric_hp2.set(4000.0f * fscale, fs);
            fric_peak.set_bpf(6500.0f * fscale, 2000.0f, fs);
        } else if (fshape == FricationShape::Palatal) {
            fric_hp.set(2000.0f * fscale, fs);
            fric_peak.set_bpf(3000.0f * fscale, 1500.0f, fs);
        }

        std::size_t k = 0;
        while (k < total) {
            std::size_t n = std::min(step_samples, total - k);
            float t = static_cast<float>(k + n / 2) / static_cast<float>(total);

            float f1 = lerp(seg.start.f1, seg.end.f1, t);
            float f2 = lerp(seg.start.f2, seg.end.f2, t);
            float f3 = lerp(seg.start.f3, seg.end.f3, t);
            float bw1 = lerp(seg.start.bw1, seg.end.bw1, t) * bw_scale;
            float bw2 = lerp(seg.start.bw2, seg.end.bw2, t) * bw_scale;
            float bw3 = lerp(seg.start.bw3, seg.end.bw3, t) * bw_scale;
            float a1 = lerp(seg.start.a1, seg.end.a1, t);
            float a2 = lerp(seg.start.a2, seg.end.a2, t);
            float a3 = lerp(seg.start.a3, seg.end.a3, t);
            float voicing = lerp(seg.start.voicing, seg.end.voicing, t);
            float frication = lerp(seg.start.frication, seg.end.frication, t);
            float f0_base = lerp(seg.start.f0_hz, seg.end.f0_hz, t);
            float aspiration = lerp(seg.start.aspiration, seg.end.aspiration, t);
            float nasal = lerp(seg.start.nasal, seg.end.nasal, t);
            float nasal_zhz = lerp(seg.start.nasal_zero_hz, seg.end.nasal_zero_hz, t);
            // 鼻音ゼロの実効周波数: nasal=0 で極位置 (打ち消し = 透過)、
            // nasal=1 で nasal_zero_hz。nasal を補間するとゼロが極から
            // 滑らかに離れていき、鼻音化の度合いが連続的に変わる。
            float zero_hz = nasal_pole_hz + nasal * (nasal_zhz - nasal_pole_hz);

            // カスケード係数はブロック長かけて補間 (係数ステップの広帯域
            // 過渡が tilt の効きを覆い隠すのを防ぐ — Resonator のコメント参照)。
            c1.set_target(f1, bw1, fs, static_cast<int>(n));
            c2.set_target(f2, bw2, fs, static_cast<int>(n));
            c3.set_target(f3, bw3, fs, static_cast<int>(n));
            nz.set_target(zero_hz, nasal_bw, fs, static_cast<int>(n));
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
                float n3 = noise() * kNoiseGain;
                // tilt はノイズ混合の後: 気息成分も一緒にやわらかくなる。
                // 帯気 (aspiration) は声帯振動と独立にカスケードを駆動するので
                // voicing はここ (カスケード入力前) で声帯成分だけに掛ける。
                // 従来は出力側で掛けていたが線形系なので等価。
                float glottal = voicing * ((1.0f - opt.breathiness) * pulse + opt.breathiness * n1);
                float voiced_src = tilt.process(glottal + aspiration * n3);

                float voiced = c5.process(c4.process(
                    c3.process(c2.process(c1.process(nz.process(np.process(voiced_src)))))));
                voiced *= voiced_amp * opt.voicing_mul * kVoicedMakeup;

                // 有声摩擦 /z/ /j/: 声門が開いている間だけ乱流が強まる物理を
                // 声門流 (openness) でノイズを振幅変調して模倣する。
                if (voicing > 0.01f) {
                    n2 *= 0.5f + 0.5f * voice.openness();
                }
                float fric_src = frication * n2 * opt.frication_mul;
                float fric;
                if (fshape == FricationShape::Formant) {
                    fric = a1 * p1.process(fric_src) + a2 * p2.process(fric_src) +
                           a3 * p3.process(fric_src);
                } else {
                    // 歯擦音: HPF で低域を落とし、広帯域ピークを足す。
                    // マスター振幅はテーブルの a3 (歯擦音では最大の成分)。
                    float hp = fric_hp.process(fric_src);
                    if (fshape == FricationShape::Sibilant) {
                        hp = fric_hp2.process(hp);
                    }
                    float shaped = hp + kFricPeakMix * fric_peak.process(hp);
                    fric = a3 * shaped *
                           ((fshape == FricationShape::Sibilant) ? kSibilantGain : kPalatalGain);
                }

                float y = (voiced + fric) * opt.gain;
                out.push_back(to_i16(y));
            }
            k += n;
        }
    }
}

}  // namespace

void render_segments(std::span<const Segment> segs, std::vector<std::int16_t>& out,
                     const Options& opt) {
    if (opt.synth == SynthVariant::Classic) {
        render_segments_classic(segs, out, opt);
        return;
    }
    render_v2(segs, out, opt);
}

}  // namespace stackchan::jtts::internal
