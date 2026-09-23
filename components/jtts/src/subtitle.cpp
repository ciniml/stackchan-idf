// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "jtts/subtitle.hpp"

#include <algorithm>
#include <cstdint>

namespace stackchan::jtts {

namespace {

bool is_pause_char(char32_t c) {
    return c == U'、' || c == U'。' || c == U'，' || c == U',' || c == U'．' || c == U'.';
}

std::size_t count_pauses(std::u32string_view s) {
    return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), is_pause_char));
}

// チャンクの読みが句読点で終わるか (末尾の空白・アクセント記号・/ は無視)。
bool ends_with_pause(std::u32string_view s) {
    for (std::size_t i = s.size(); i > 0; --i) {
        const char32_t c = s[i - 1];
        if (c == U' ' || c == U'　' || c == U'\n' || c == U'\'' || c == U'’' || c == U'/') continue;
        return is_pause_char(c);
    }
    return false;
}

// UTF-8 で s[i] から始まる句読点の長さ (バイト)。句読点でなければ 0。
std::size_t pause_len_utf8(std::string_view s, std::size_t i) {
    const auto b = [&](std::size_t k) { return i + k < s.size() ? static_cast<std::uint8_t>(s[i + k]) : 0u; };
    if (b(0) == ',' || b(0) == '.') return 1;
    if (b(0) == 0xE3 && b(1) == 0x80 && (b(2) == 0x81 || b(2) == 0x82)) return 3;  // 、 。
    if (b(0) == 0xEF && b(1) == 0xBC && (b(2) == 0x8C || b(2) == 0x8E)) return 3;  // ， ．
    return 0;
}

}  // namespace

SubtitleMapper::SubtitleMapper(std::string_view display_utf8, std::u32string_view reading) : whole_(display_utf8) {
    // 表示テキストを句読点の直後で区切る (句読点は前の句に含める)。
    std::string cur;
    std::size_t pauses = 0;
    for (std::size_t i = 0; i < display_utf8.size();) {
        const std::size_t n = pause_len_utf8(display_utf8, i);
        if (n > 0) {
            cur.append(display_utf8.substr(i, n));
            segments_.push_back(std::move(cur));
            cur.clear();
            ++pauses;
            i += n;
        } else {
            cur.push_back(display_utf8[i]);
            ++i;
        }
    }
    segments_.push_back(std::move(cur));  // 最後の句読点より後ろ (空のこともある)
    mapped_ = !display_utf8.empty() && pauses == count_pauses(reading);
}

std::string SubtitleMapper::next(std::u32string_view chunk_reading) {
    const bool first = first_;
    first_ = false;
    if (!mapped_) {
        return first ? whole_ : std::string{};
    }
    const std::size_t pauses = count_pauses(chunk_reading);
    const std::size_t begin = consumed_;
    const std::size_t after = consumed_ + pauses;
    consumed_ = after;
    // 句読点で終わるチャンクは、その句読点を含む句までが担当 (次のチャンクは次の句から)。
    // 途中で切れたチャンクは、まだ句読点に届いていない句 (after) も担当する。
    const std::size_t last = segments_.size() - 1;
    const std::size_t a = std::min(begin, last);
    const std::size_t b = std::min(pauses > 0 && ends_with_pause(chunk_reading) ? after - 1 : after, last);
    std::string out;
    for (std::size_t k = a; k <= b; ++k) out += segments_[k];
    return out;
}

}  // namespace stackchan::jtts
