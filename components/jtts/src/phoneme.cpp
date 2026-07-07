// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include <algorithm>

#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

FormantFrame silent_frame(float f0) {
    FormantFrame s;
    s.voicing = 0.0f;
    s.frication = 0.0f;
    s.a1 = s.a2 = s.a3 = 0.0f;
    s.f0_hz = f0;
    return s;
}

// prenasal_zero_hz > 0 のとき、後続モーラが「ん」なので母音末尾 ~30 ms で
// nasal を 0→0.5 に立ち上げる (先行母音の鼻音化。ん への移行でスペクトルが
// 急変するのを防ぎ、自然な渡りになる)。値は後続の鼻音ゼロ周波数。
void add_cv_segments(Consonant c, Vowel v, bool palatalized, bool devoiced, float mora_ms,
                     float f0, float prenasal_zero_hz, std::vector<Segment>& out) {
    FormantFrame vowel = vowel_frame(v, palatalized);
    vowel.f0_hz = f0;

    if (devoiced) {
        // 母音を囁き化: 声帯駆動を切り、ノイズで母音フォルマントを軽く励起する。
        // 振幅も下げて短く弱く感じさせる。
        vowel.voicing = 0.0f;
        vowel.frication = 0.35f;
        vowel.a1 *= 0.55f;
        vowel.a2 *= 0.55f;
        vowel.a3 *= 0.55f;
    }

    // 母音末尾セグメントを積む。prenasal (次モーラが「ん」) なら末尾 ~30 ms
    // で nasal を 0→0.5 に上げて先行母音を鼻音化する。
    auto push_vowel_tail = [&](float consumed) {
        float v_ms = std::max(20.0f, mora_ms - consumed);
        if (prenasal_zero_hz > 0.0f) {
            FormantFrame nasalized = vowel;
            nasalized.nasal = 0.5f;
            nasalized.nasal_zero_hz = prenasal_zero_hz;
            const float ramp_ms = std::min(30.0f, v_ms);
            if (v_ms > ramp_ms) {
                out.push_back({vowel, vowel, v_ms - ramp_ms});
            }
            FormantFrame head = vowel;
            out.push_back({head, nasalized, ramp_ms});
            return;
        }
        out.push_back({vowel, vowel, v_ms});
    };

    if (c == Consonant::None) {
        push_vowel_tail(0.0f);
        return;
    }

    FormantFrame burst = consonant_burst(c, v);
    burst.f0_hz = f0;
    if (palatalized && !is_nasal(c)) {
        burst.f2 += 200.0f;
    }

    const FormantFrame silence = silent_frame(f0);

    if (is_voiceless_stop(c)) {
        // 閉鎖 → 短い破裂バースト → 帯気 (VOT) → 有声立ち上がり。
        // VOT は調音位置依存 (唇音ほど短く軟口蓋音ほど長い): p=15, t=25, k=45 ms。
        // 帯気区間は声帯振動なしで後続母音のフォルマント (カスケード) を
        // ノイズ駆動する — /ka/ の「はぁ」っぽい立ち上がりの正体。
        const float vot_ms = (c == Consonant::P) ? 15.0f : (c == Consonant::T) ? 25.0f : 45.0f;
        out.push_back({silence, silence, 30.0f});
        out.push_back({burst, burst, 5.0f});
        FormantFrame asp = vowel;
        asp.voicing = 0.0f;
        asp.frication = 0.0f;
        asp.aspiration = 0.6f;
        FormantFrame asp_end = asp;
        asp_end.aspiration = 0.4f;
        out.push_back({asp, asp_end, vot_ms});
        // 有声の立ち上がり: voicing 0→1、帯気の残りは消える。
        FormantFrame onset = vowel;
        onset.voicing = 0.0f;
        onset.aspiration = 0.4f;
        out.push_back({onset, vowel, 20.0f});
        push_vowel_tail(55.0f + vot_ms);
    } else if (is_voiced_stop(c)) {
        // 有声破裂音は閉鎖中も声帯が振動する (voice bar): 低い F1 だけが
        // 壁越しに漏れる低振幅の唸り。これが無いと /g d b/ が /k t p/ の
        // 弱い版にしか聞こえない。
        FormantFrame voice_bar = silent_frame(f0);
        voice_bar.f1 = 200.0f;
        voice_bar.bw1 = 100.0f;
        voice_bar.f2 = vowel.f2;
        voice_bar.f3 = vowel.f3;
        voice_bar.bw2 = 300.0f;  // 高次は広帯域幅でぼかす (カスケードでは
        voice_bar.bw3 = 400.0f;  // a2=a3=0 にできないため)
        voice_bar.voicing = 0.6f;
        voice_bar.a1 = 0.25f;
        out.push_back({voice_bar, voice_bar, 35.0f});
        out.push_back({burst, burst, 5.0f});
        out.push_back({burst, vowel, 25.0f});
        push_vowel_tail(65.0f);
    } else if (is_voiceless_fric(c)) {
        // 立ち上がりを 25 ms かけて滑らかに上げる。
        // これを入れないと /ʃ/ /s/ のノイズが突然全振幅で出て破擦音 /tʃ/
        // /ts/ と区別がつかなくなる (「した」が「ちた」に聞こえる現象)。
        FormantFrame burst_soft = burst;
        burst_soft.frication = 0.25f;
        burst_soft.a3 *= 0.40f;
        out.push_back({burst_soft, burst, 25.0f});
        out.push_back({burst, burst, 45.0f});
        out.push_back({burst, vowel, 20.0f});
        push_vowel_tail(90.0f);
    } else if (is_voiced_fric_affric(c)) {
        out.push_back({burst, burst, 50.0f});
        out.push_back({burst, vowel, 25.0f});
        push_vowel_tail(75.0f);
    } else if (is_affricate(c)) {
        out.push_back({silence, silence, 25.0f});
        out.push_back({burst, burst, 40.0f});
        out.push_back({burst, vowel, 20.0f});
        push_vowel_tail(85.0f);
    } else if (is_nasal(c)) {
        FormantFrame nf = nasal_frame(c);
        nf.f0_hz = f0;
        out.push_back({nf, nf, 50.0f});
        out.push_back({nf, vowel, 20.0f});
        push_vowel_tail(70.0f);
    } else if (is_glide(c)) {
        out.push_back({burst, vowel, 50.0f});
        push_vowel_tail(50.0f);
    } else if (c == Consonant::R) {
        out.push_back({burst, vowel, 25.0f});
        push_vowel_tail(25.0f);
    } else {
        out.push_back({vowel, vowel, mora_ms});
    }
}

}  // namespace

void build_segments(std::span<const Mora> moras, std::vector<Segment>& out, const Options& opt) {
    out.clear();
    const float f0 = opt.f0_hz;
    const float mora_ms = opt.mora_ms;

    // i 番目が「ん」のときのフレーム。次モーラの子音への調音位置同化:
    // m/b/p の前では両唇鼻音 [m]、それ以外は歯茎〜口蓋垂。単独・語末の
    // 「ん」は口蓋垂鼻音 [N] で鼻音ゼロがさらに高い (1800 Hz)。
    auto moraic_n_frame = [&](std::size_t i) {
        Consonant nasal_c = Consonant::N;
        bool assimilated = false;
        if (i + 1 < moras.size() && moras[i + 1].kind == MoraKind::CV) {
            Consonant nc = moras[i + 1].c;
            if (nc == Consonant::M || nc == Consonant::B || nc == Consonant::P) {
                nasal_c = Consonant::M;
                assimilated = true;
            }
        }
        FormantFrame nf = nasal_frame(nasal_c);
        if (!assimilated) {
            nf.nasal_zero_hz = 1800.0f;
        }
        nf.f0_hz = f0;
        return nf;
    };

    for (std::size_t i = 0; i < moras.size(); ++i) {
        const Mora& m = moras[i];
        switch (m.kind) {
            case MoraKind::CV: {
                // 次が「ん」なら母音末尾を鼻音化する (ゼロ周波数は ん 側と揃える)。
                float prenasal_zero_hz = 0.0f;
                if (i + 1 < moras.size() && moras[i + 1].kind == MoraKind::MoraicN) {
                    prenasal_zero_hz = moraic_n_frame(i + 1).nasal_zero_hz;
                }
                add_cv_segments(m.c, m.v, m.palatalized, m.devoiced, mora_ms, f0,
                                prenasal_zero_hz, out);
                break;
            }
            case MoraKind::MoraicN: {
                FormantFrame nf = moraic_n_frame(i);
                out.push_back({nf, nf, mora_ms});
                break;
            }
            case MoraKind::Sokuon: {
                FormantFrame s = silent_frame(f0);
                out.push_back({s, s, 70.0f});
                break;
            }
            case MoraKind::Chouon: {
                if (!out.empty()) {
                    FormantFrame ref = out.back().end;
                    out.push_back({ref, ref, mora_ms});
                }
                break;
            }
        }
    }
}

void build_segments(std::span<const Mora>, std::span<Segment>) {}

void apply_devoicing(std::vector<Mora>& moras) {
    for (std::size_t i = 0; i < moras.size(); ++i) {
        Mora& m = moras[i];
        if (m.kind != MoraKind::CV) continue;
        if (m.v != Vowel::I && m.v != Vowel::U) continue;
        if (!is_voiceless_consonant(m.c)) continue;

        // 「次が無声」かどうかを判定。CV なら子音、Sokuon (っ) は無声子音前置の
        // マーカなので常に無声扱い、Chouon (長音) は母音延長なので有声扱い、
        // MoraicN (ん) は有声扱い、末尾 (i+1==N) は文末でやはり無声化が起こる。
        bool next_voiceless = false;
        if (i + 1 == moras.size()) {
            next_voiceless = true;
        } else {
            const Mora& nx = moras[i + 1];
            switch (nx.kind) {
                case MoraKind::CV:
                    next_voiceless = is_voiceless_consonant(nx.c);
                    break;
                case MoraKind::Sokuon:
                    next_voiceless = true;
                    break;
                case MoraKind::Chouon:
                case MoraKind::MoraicN:
                    next_voiceless = false;
                    break;
            }
        }

        if (next_voiceless) {
            m.devoiced = true;
        }
    }
}

}  // namespace stackchan::jtts::internal
