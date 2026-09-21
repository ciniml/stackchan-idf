// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS の音素 ID (saan_g2p の出力) と音素ごとの継続長 (d_hat) から、口形の区間列を作る
// (sanoTTS のコアには依存しない: 語彙は上流 g2p_table.h の生徒インデックス)。
#include <cstdint>
#include <vector>

#include "internal.hpp"

namespace stackchan::jtts::internal {

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

}  // namespace stackchan::jtts::internal
