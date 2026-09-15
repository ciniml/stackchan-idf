// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// かな (jtts 記法) → sanoTTS-jp かな中間表現。
//
// sanoTTS-jp の端末側 G2P (components/saanotts/upstream/g2p.h) は
//   ひらがな + `[` (上昇) `]` (下降核) `#` (アクセント句境界) `_` (ポーズ)
//   + `°` (無声化) + `っ` `ん` `ー`
// を受け取り、`^` (BOS) と `$` (EOS) は自分で付ける。上流の例:
//   今日は良い天気ですね → きょ][おわよ][いて][んきです°ね
// つまり `[` はその位置からピッチが上がり、`]` はその直後で下がる。
//
// jtts のかな記法 (HMM エンジンと共通、hts_label.cpp):
//   `'` = 直前モーラがアクセント核、`/` = アクセント句境界、「、」= ポーズ、
//   「。」= 文境界。記号なしのアクセント句は平板型。
//
// 東京式の写像 (n モーラ、核 k):
//   k == 1 (頭高)  : m1 ] m2 … mn
//   k >= 2 (中高/尾高): m1 [ m2 … mk ] …
//   k == 0 (平板)  : m1 [ m2 … mn      (下降なし)
//   n == 1 で平板   : 記号なし
// `#` は上流の例が `][` 連続で表現しているので v1 では出さない。
// `°` (無声化) は付けない — 上流 M-14 で「無声子音に挟まれた i/u」の規則は
// 過剰無声化すると実測されている。

#include <string>
#include <string_view>
#include <vector>

#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

// カタカナ (U+30A1..U+30F6) → ひらがな (U+3041..U+3096)。
char32_t to_hiragana(char32_t c) {
    if (c >= U'ァ' && c <= U'ヶ') return static_cast<char32_t>(c - 0x60);
    return c;
}

bool is_hiragana(char32_t c) {
    return c >= U'ぁ' && c <= U'ゖ';
}

// 直前のモーラにくっつく小書き文字。
bool is_small(char32_t c) {
    switch (c) {
    case U'ぁ': case U'ぃ': case U'ぅ': case U'ぇ': case U'ぉ':
    case U'ゃ': case U'ゅ': case U'ょ': case U'ゎ':
        return true;
    default:
        return false;
    }
}

bool is_pause(char32_t c) {
    return c == U'、' || c == U'，' || c == U',' || c == U'；' || c == U';' || c == U'：' || c == U':';
}

bool is_sentence_end(char32_t c) {
    return c == U'。' || c == U'．' || c == U'.' || c == U'！' || c == U'!' || c == U'？' || c == U'?';
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

struct Phrase {
    std::vector<std::u32string> moras;
    int nucleus = 0;  // 1 始まり。0 = 平板
};

void emit_phrase(const Phrase& p, std::string& out) {
    const int n = static_cast<int>(p.moras.size());
    if (n == 0) return;
    const int k = (p.nucleus >= 1 && p.nucleus <= n) ? p.nucleus : 0;
    for (int i = 0; i < n; ++i) {
        for (char32_t c : p.moras[i]) append_utf8(out, c);
        if (i == 0 && k == 1) {
            out.push_back(']');
        } else if (i == 0 && n >= 2) {
            out.push_back('[');
        } else if (k >= 2 && i == k - 1) {
            out.push_back(']');
        }
    }
}

}  // namespace

bool build_sano_ir(std::u32string_view text, std::string& ir_utf8) {
    ir_utf8.clear();
    std::vector<Phrase> phrases(1);
    // ポーズを出す位置: 句の前に `_` を挟むフラグ。
    std::vector<bool> pause_before(1, false);
    bool any_mora = false;

    auto close_phrase = [&](bool pause) {
        phrases.emplace_back();
        pause_before.push_back(pause);
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = to_hiragana(text[i]);
        Phrase& ph = phrases.back();
        if (c == U'/' ) {
            if (!ph.moras.empty()) close_phrase(false);
            continue;
        }
        if (c == U'\'' || c == U'’') {
            if (!ph.moras.empty() && ph.nucleus == 0) ph.nucleus = static_cast<int>(ph.moras.size());
            continue;
        }
        if (is_pause(c)) {
            if (!ph.moras.empty()) close_phrase(true);
            else if (pause_before.size() > 1) pause_before.back() = true;
            continue;
        }
        if (is_sentence_end(c)) {
            if (!ph.moras.empty()) close_phrase(true);
            continue;
        }
        if (c == U'ー' || c == U'っ' || c == U'ん') {
            ph.moras.emplace_back(1, c);
            any_mora = true;
            continue;
        }
        if (is_small(c)) {
            if (!ph.moras.empty()) ph.moras.back().push_back(c);
            continue;  // 先頭の小書きは捨てる
        }
        if (is_hiragana(c)) {
            ph.moras.emplace_back(1, c);
            any_mora = true;
            continue;
        }
        // 空白・未知の文字は読み飛ばす (G2P が知らない文字を渡すと全体が失敗する)。
    }
    if (!any_mora) return false;

    bool first = true;
    for (std::size_t i = 0; i < phrases.size(); ++i) {
        if (phrases[i].moras.empty()) continue;
        if (!first && pause_before[i]) ir_utf8.push_back('_');
        emit_phrase(phrases[i], ir_utf8);
        first = false;
    }
    return !ir_utf8.empty();
}

}  // namespace stackchan::jtts::internal
