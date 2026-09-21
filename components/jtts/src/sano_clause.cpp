// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS を句ごとに合成するときの境界の整形 (sanoTTS のコアには依存しない)。
// 発話全体を 1 回で合成すると、「、」「。」はモデルが決める短いポーズになって句の区切りが
// 弱くなるので、HMM と同じく句読点ごとに 1 句ずつ合成し、句間に明示的な無音を入れる。
// 重み (非 MIT の blob) が無くてもホストでテストできるよう、1 句の合成は関数で受け取る。
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

// 句間の無音 (等速時)。HMM の pau は「両側の sil の残り 210 ms ずつ」(hmm_synth.cpp の
// kPauseHalfMs) で、実測でも ≈ 0.42 s / 速度。同じ長さにして、HMM と同じ間で話す。
constexpr float kClausePauseMs = 420.0f;

}  // namespace

SilenceTrim trim_silence(std::vector<std::int16_t>& pcm, std::uint32_t rate_hz, bool lead, bool trail,
                         std::uint32_t keep_ms) {
    SilenceTrim removed;
    if (pcm.empty() || (!lead && !trail)) return removed;
    int peak = 0;
    for (std::int16_t v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
    const int thr = std::max(48, peak / 100);
    std::size_t first = 0;
    while (first < pcm.size() && std::abs(static_cast<int>(pcm[first])) < thr) ++first;
    if (first == pcm.size()) return removed;  // 全体が無音: 触らない
    std::size_t last = pcm.size();            // 最後の音の次
    while (last > first && std::abs(static_cast<int>(pcm[last - 1])) < thr) --last;

    const std::size_t keep = static_cast<std::size_t>(keep_ms) * rate_hz / 1000u;
    const std::size_t begin = lead ? (first > keep ? first - keep : 0) : 0;
    const std::size_t end = trail ? std::min(pcm.size(), last + keep) : pcm.size();
    removed.back = pcm.size() - end;
    removed.front = begin;
    pcm.erase(pcm.begin() + static_cast<std::ptrdiff_t>(end), pcm.end());
    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(begin));
    return removed;
}

void crop_spans(std::vector<VisemeSpan>& spans, float drop_front_ms, float keep_ms) {
    std::vector<VisemeSpan> out;
    float t = 0.0f;                       // 元の時間軸での現在位置
    const float keep_end = drop_front_ms + keep_ms;
    for (const VisemeSpan& s : spans) {
        const float b = std::max(t, drop_front_ms);
        const float e = std::min(t + s.duration_ms, keep_end);
        if (e > b) out.push_back({s.vowel, e - b});
        t += s.duration_ms;
    }
    spans = std::move(out);
}

// --- 音素 ID → 口形 (語彙は上流 g2p_table.h の生徒インデックス。仮名表との一致は
//     test_sano_clause が kSaanG2pMora で検証する) ---
namespace {

constexpr int kIdPad = 0;
constexpr int kIdBos = 1;
constexpr int kIdEos = 2;
constexpr int kIdVowelLo = 10;  // a i u e o = 10..14
constexpr int kIdVowelHi = 14;

// マーク ? # [ ] (アクセント / 疑問の記号。音素の間に入る余白として扱う)。
bool is_mark(int id) { return id == 3 || (id >= 7 && id <= 9); }

// 両唇音 (唇を閉じる): p py b by m my。
bool is_bilabial(int id) { return id == 35 || id == 36 || id == 37 || id == 38 || id == 51 || id == 52; }

Vowel vowel_of_id(int id) {
    switch (id) {
        case 10: return Vowel::A;
        case 11: return Vowel::I;
        case 12: return Vowel::U;
        case 13: return Vowel::E;
        case 14: return Vowel::O;
        default: return Vowel::None;
    }
}

}  // namespace

void sano_ids_to_spans(const std::int32_t* ids, const std::int32_t* d_hat, std::int32_t n, float ms_per_frame,
                       std::vector<VisemeSpan>& out) {
    out.clear();
    if (ids == nullptr || d_hat == nullptr || n <= 0) return;

    // 音素 (PAD / マーク以外) の位置を先に拾い、直後の音素の母音を引けるようにする。
    auto is_blank = [&](std::int32_t i) { return ids[i] == kIdPad || is_mark(ids[i]); };
    Vowel held = Vowel::None;  // 直前の音素の形 (PAD / マークの間はこれを保つ)
    bool prev_pad = false;     // 直前のトークンが PAD (連続した PAD の 2 つ目以降 = ポーズ)
    for (std::int32_t i = 0; i < n; ++i) {
        const int id = ids[i];
        Vowel v;
        if (id == kIdBos || id == kIdEos) {
            v = Vowel::None;
            held = Vowel::None;
            prev_pad = false;
        } else if (is_blank(i)) {
            const bool pause = id == kIdPad && prev_pad;
            v = pause ? Vowel::None : held;
            prev_pad = id == kIdPad;
        } else {
            prev_pad = false;
            if (id >= kIdVowelLo && id <= kIdVowelHi) {
                v = vowel_of_id(id);
            } else if (is_bilabial(id)) {
                v = Vowel::None;
            } else if (id >= 15 && id <= 24) {
                v = Vowel::None;  // 無声化母音 (15..19)・ん (20..23)・っ (24)
            } else {
                // その他の子音: 直後の (PAD / マークを飛ばした) 音素が平母音ならその形を先取り。
                v = Vowel::None;
                for (std::int32_t j = i + 1; j < n; ++j) {
                    if (is_blank(j)) continue;
                    if (ids[j] >= kIdVowelLo && ids[j] <= kIdVowelHi) v = vowel_of_id(ids[j]);
                    break;
                }
            }
            held = v;
        }
        const float ms = static_cast<float>(d_hat[i]) * ms_per_frame;
        if (ms <= 0.0f) continue;
        if (!out.empty() && out.back().vowel == v) {
            out.back().duration_ms += ms;
        } else {
            out.push_back({v, ms});
        }
    }
}

StreamOutcome stream_sano_clauses(std::u32string_view text, const Options& opt, const SanoClauseSynth& synth_one,
                                  const SanoChunkFn& emit) {
    std::vector<HmmChunk> clauses;
    if (!split_clauses(text, clauses)) return StreamOutcome::NoOutput;

    // mora_ms は 1 モーラの長さ。110 ms = 等速 (sanoTTS の s_v / HMM の speed と同じ写像)。
    const float scale = std::clamp(opt.mora_ms / 110.0f, 0.5f, 2.0f);
    const float pause_ms = kClausePauseMs * scale;

    std::size_t emitted = 0;
    for (std::size_t k = 0; k < clauses.size(); ++k) {
        std::vector<std::int16_t> pcm;
        std::vector<VisemeSpan> spans;
        std::uint32_t rate = 0;
        const ClauseResult r = synth_one(clauses[k].text, pcm, rate, spans);
        if (r == ClauseResult::Skip) continue;
        if (r == ClauseResult::Fail || pcm.empty() || rate == 0) {
            return emitted > 0 ? StreamOutcome::Aborted : StreamOutcome::NoOutput;
        }
        const bool has_prev = emitted > 0;
        const bool has_next = k + 1 < clauses.size();
        // 内側の境界: モデルが付けた前後の無音は切り詰めて、こちらの無音に置き換える。
        // 発話の先頭 / 末尾は従来どおり (切り詰めない)。口形の区間も同じだけ切り詰める。
        const SilenceTrim cut = trim_silence(pcm, rate, /*lead=*/has_prev, /*trail=*/has_next);
        const float ms_per_sample = 1000.0f / static_cast<float>(rate);
        if (!spans.empty() && (cut.front != 0 || cut.back != 0)) {
            crop_spans(spans, static_cast<float>(cut.front) * ms_per_sample,
                       static_cast<float>(pcm.size()) * ms_per_sample);
        }
        if (has_next) {
            const auto pause_samples = static_cast<std::size_t>(pause_ms * static_cast<float>(rate) / 1000.0f);
            pcm.resize(pcm.size() + pause_samples, 0);
            if (!spans.empty()) spans.push_back({Vowel::None, static_cast<float>(pause_samples) * ms_per_sample});
        }
        ++emitted;
        if (!emit(std::move(pcm), rate, std::move(spans), clauses[k].text)) return StreamOutcome::Cancelled;
    }
    return emitted > 0 ? StreamOutcome::Ok : StreamOutcome::NoOutput;
}

}  // namespace stackchan::jtts::internal
