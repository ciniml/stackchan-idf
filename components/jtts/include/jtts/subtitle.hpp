// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace stackchan::jtts {

// 吹き出し (表示テキスト) を、合成チャンクの再生に同期させるための対応付け。
//
// jtts は「読み」(かな) しか持たず、表示テキスト (漢字まじり) との対応は知らない。
// しかしチャンクは句読点 (、。，,．.) を境に分けられるので、表示テキストも同じ句読点で
// 区切れば「n 個目の句読点までの部分」が対応する。読みと表示の句読点の数が合わない
// ときは対応を諦め、最初のチャンクで全文を出す。
//
//   SubtitleMapper m(display_utf8, reading);
//   synthesize_stream(reading, [&](SynthChunk&& c) {
//       std::string text = m.next(c.text);   // このチャンクの間に出す表示テキスト
//       ...
//   });
class SubtitleMapper {
public:
    SubtitleMapper(std::string_view display_utf8, std::u32string_view reading);

    // 句読点で対応が取れるか。false のときは next() が最初に全文を返し、以降は空を返す。
    bool mapped() const { return mapped_; }

    // 次のチャンク (その読み) の間に出す表示テキスト。チャンクは先頭から順に渡すこと。
    // 句読点で終わるチャンクはその句までを、途中で切れたチャンクは続きの句を受け持つ
    // (強制分割で同じ句を 2 チャンクにまたがって出す場合は同じ文字列が返る)。
    // 出すものが無ければ空。
    std::string next(std::u32string_view chunk_reading);

private:
    std::string whole_;
    std::vector<std::string> segments_;  // 表示テキストを句読点の直後で区切ったもの (句読点を含む)
    bool mapped_ = false;
    bool first_ = true;
    std::size_t consumed_ = 0;  // ここまでのチャンクが消費した句読点の数
};

}  // namespace stackchan::jtts
