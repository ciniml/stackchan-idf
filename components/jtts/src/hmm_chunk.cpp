// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HMM エンジン用のテキスト分割。hts_engine は 1 発話の合成中、フレーム数に
// 比例した量 (≈ 2 KB / 5 ms フレーム) の作業メモリを確保し続けるため、長い
// フレーズをそのまま合成すると PSRAM を使い切って確保失敗 → クラッシュする。
// そこで発話を、メモリ予算に収まるモーラ数のチャンクに分けて順に合成する。
//
// 分割位置の優先順位:
//   1. 句読点 (、。，,．.)  … アクセント句 + 呼気段落の境界。元々ポーズが入る所
//   2. アクセント句境界 (/)  … ポーズなし
//   3. 最後の手段: モーラ境界 (1 つの句がそれ単体で予算を超える場合)。
//      拗音・長音・アクセント核の直前では切らない。
#include <algorithm>
#include <string>
#include <vector>

#include "internal.hpp"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace stackchan::jtts::internal {

namespace {

bool is_pause_char(char32_t c) {
    return c == U'、' || c == U'。' || c == U'，' || c == U',' || c == U'．' || c == U'.';
}

bool is_accent_mark(char32_t c) {
    return c == U'\'' || c == U'’';
}

// 直前のモーラと一体になる文字 (この直前では切らない)。
bool is_attached_to_prev(char32_t c) {
    switch (c) {
        case U'ゃ': case U'ゅ': case U'ょ': case U'ぁ': case U'ぃ': case U'ぅ': case U'ぇ': case U'ぉ':
        case U'ゎ': case U'ャ': case U'ュ': case U'ョ': case U'ァ': case U'ィ': case U'ゥ': case U'ェ':
        case U'ォ': case U'ヮ': case U'ー': case U'/':
            return true;
        default:
            return is_accent_mark(c);
    }
}

std::size_t count_moras(std::u32string_view s) {
    std::vector<Mora> moras;
    return parse_kana(s, moras) ? moras.size() : 0;
}

struct Unit {
    std::u32string text;
    std::size_t moras = 0;
    bool pause = false;  // 末尾が句読点 (= 直後にポーズが入る)
};

// 1 つの句が max_moras を超えるとき、モーラ境界で切って units に積む。
// 切った断片ではアクセント核 (') の位置が意味を失うので取り除く (平板になる)。
void push_split_unit(const Unit& u, std::size_t max_moras, std::vector<Unit>& units) {
    std::u32string rest;
    for (char32_t c : u.text) {
        if (!is_accent_mark(c)) rest.push_back(c);
    }
    while (count_moras(rest) > max_moras) {
        // max_moras 以内に収まる最長の接頭辞のうち、切ってよい位置を探す。
        std::size_t cut = 0;
        std::size_t fallback = 0;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            if (count_moras(std::u32string_view(rest).substr(0, i)) > max_moras) break;
            fallback = i;
            if (!is_attached_to_prev(rest[i])) cut = i;
        }
        if (cut == 0) cut = fallback;
        if (cut == 0) break;  // 1 モーラで既に予算超過: これ以上は割れない
        Unit piece;
        piece.text = rest.substr(0, cut);
        piece.moras = count_moras(piece.text);
        units.push_back(std::move(piece));
        rest.erase(0, cut);
    }
    Unit last;
    last.text = std::move(rest);
    last.moras = count_moras(last.text);
    last.pause = u.pause;
    units.push_back(std::move(last));
}

// テスト用: 0 以外なら「PCM に使える PSRAM」をこの値 [byte] とみなす。
std::size_t g_pcm_free_override = 0;

}  // namespace

float estimate_utterance_ms(std::u32string_view text, float mora_ms) {
    // 実測: 先頭 + 末尾の sil ≈ 0.65 s、句読点の pau ≈ 0.46 s、1 モーラ ≈ 1.25 × mora_ms
    // (HMM: 等速時 0.138〜0.150 s、フォルマントは mora_ms + 子音分)。上限寄りに見積もる。
    std::size_t pauses = 0;
    for (char32_t c : text) {
        if (is_pause_char(c)) ++pauses;
    }
    return 700.0f + 500.0f * static_cast<float>(pauses) +
           1.3f * mora_ms * static_cast<float>(count_moras(text));
}

bool pcm_fits_in_memory(std::size_t samples) {
    const std::size_t bytes = samples * sizeof(std::int16_t);
    if (g_pcm_free_override != 0) return bytes * 2 + 256 * 1024 <= g_pcm_free_override;
#if defined(ESP_PLATFORM)
    // PCM は 1 つの連続ブロック (std::vector) で PSRAM に置く。合成中の他の確保
    // (hts_engine の作業メモリ、再生バッファ) と一時的な倍増に備えて 2 倍 + 256 KB を要求する。
    const std::size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const std::size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    return bytes <= largest && bytes * 2 + 256 * 1024 <= free_psram;
#else
    return true;
#endif
}

void set_pcm_memory_limit_for_test(std::size_t bytes) { g_pcm_free_override = bytes; }

bool split_hmm_text(std::u32string_view text, std::size_t max_moras, std::vector<HmmChunk>& out,
                    std::size_t first_moras) {
    out.clear();
    if (max_moras == 0) return false;
    const bool ramp = first_moras > 0;

    // 全体が収まるなら手を加えず 1 チャンク (従来と完全に同じ入力で合成する)。
    // 低遅延モードでは句読点で分けて先頭を早く出したいので、この近道は使わない
    // (句の途中では切らないので、句読点の無い短文は結局 1 チャンクになる)。
    const std::size_t total = count_moras(text);
    if (total == 0) return false;
    if (!ramp && total <= max_moras) {
        out.push_back({std::u32string(text), false, total});
        return true;
    }

    // 句読点 / アクセント句境界で「句」に分ける。区切り文字は前の句に含める。
    std::vector<Unit> units;
    Unit cur;
    auto flush_unit = [&](bool pause) {
        if (cur.text.empty()) return;
        cur.moras = count_moras(cur.text);
        cur.pause = pause;
        if (cur.moras > max_moras) {
            push_split_unit(cur, max_moras, units);
        } else {
            units.push_back(std::move(cur));
        }
        cur = Unit{};
    };
    for (char32_t c : text) {
        cur.text.push_back(c);
        if (is_pause_char(c)) {
            flush_unit(true);
        } else if (c == U'/') {
            flush_unit(false);
        }
    }
    flush_unit(false);

    // 予算に収まるまで句を貪欲に詰める。低遅延モードでは上限を first_moras から
    // 始め、チャンクを閉じるたびに直前のチャンクの 1.3 倍へ引き上げる。
    std::size_t cap = ramp ? std::min(first_moras, max_moras) : max_moras;
    HmmChunk chunk;
    auto flush_chunk = [&] {
        const std::size_t done = chunk.moras;
        if (chunk.moras > 0) out.push_back(std::move(chunk));
        chunk = HmmChunk{};
        if (ramp && done > 0) {
            const std::size_t grown = (done * 13 + 9) / 10;  // ceil(done * 1.3)
            cap = std::min(max_moras, std::max(first_moras, grown));
        }
    };
    for (auto& u : units) {
        if (chunk.moras > 0 && chunk.moras + u.moras > cap) flush_chunk();
        chunk.text += u.text;
        chunk.moras += u.moras;
        chunk.pause_after = u.pause;
    }
    flush_chunk();
    return !out.empty();
}

bool split_clauses(std::u32string_view text, std::vector<HmmChunk>& out) {
    out.clear();
    std::u32string cur;
    auto flush = [&](bool pause) {
        if (cur.empty()) return;
        const std::size_t moras = count_moras(cur);
        if (moras == 0) {
            // 読める内容が無い断片 (記号だけなど) は前の句に含める。先頭なら次の句の前に残す。
            if (!out.empty()) {
                out.back().text += cur;
                out.back().pause_after = out.back().pause_after || pause;
                cur.clear();
            }
            return;
        }
        HmmChunk c;
        c.text = std::move(cur);
        c.moras = moras;
        c.pause_after = pause;
        out.push_back(std::move(c));
        cur.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        cur.push_back(text[i]);
        if (!is_pause_char(text[i])) continue;
        // 句読点が続く場合 (、、 や 。。。) は同じ句にまとめる。
        while (i + 1 < text.size() && is_pause_char(text[i + 1])) cur.push_back(text[++i]);
        flush(true);
    }
    flush(false);
    return !out.empty();
}

}  // namespace stackchan::jtts::internal
