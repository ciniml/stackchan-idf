// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 吹き出しの文字列の表示位置 (スクロール) の計算。描画から切り離した純粋な関数で、
// ホストでテストできる。
//
//   - 文字列が吹き出しに収まるなら、左詰めで静止表示する (スクロールしない)。
//   - 溢れるときだけスクロールする: 最初は左詰め (文頭が見える) で少し止まり、
//     溢れた分だけ左へ動かして、文末が見えたところで止める。ループはしない。
//   - 表示時間 (hold_ms) が分かっているときは、その間に文末まで読めるよう速度を
//     合わせる。チャンクごとに切り替わる吹き出しなら、次のチャンクが始まるまでに
//     文末が見える。
#pragma once

#include <algorithm>
#include <cstdint>

namespace stackchan::avatar::internal {

// 収まる文字列を最低限見せておく時間 (hold_ms 未指定のとき)。
constexpr std::uint32_t kBalloonStaticHoldMs = 3000;
// 溢れる文字列: スクロール前後の静止時間の上限と、既定 / 上限 / 下限のスクロール速度。
constexpr std::uint32_t kBalloonEdgeHoldMs = 1000;
constexpr std::int32_t kBalloonScrollPxPerSec = 60;
constexpr std::int32_t kBalloonScrollMaxPxPerSec = 120;
constexpr std::int32_t kBalloonScrollMinPxPerSec = 30;

struct BalloonScroll {
    std::int32_t offset_px = 0;  // 左へ動かした量 (0 = 左詰め、travel = 文末が右端に揃う)
    bool done = false;           // 表示を終えてよい (呼び出し側が吹き出しを閉じる)
};

// text_w: 文字列の幅、inner_w: 吹き出しの内側の幅 [px]。
// elapsed_ms: 表示してからの時間、hold_ms: 呼び出し側の指定表示時間 (0 = 未指定)。
constexpr BalloonScroll compute_balloon_scroll(std::int32_t text_w, std::int32_t inner_w, std::uint32_t elapsed_ms,
                                               std::uint32_t hold_ms)
{
    if (text_w <= inner_w) {
        return {0, elapsed_ms >= std::max(hold_ms, kBalloonStaticHoldMs)};
    }
    const auto travel = static_cast<std::uint32_t>(text_w - inner_w);
    const std::uint32_t default_ms = travel * 1000u / kBalloonScrollPxPerSec;
    const std::uint32_t min_ms = travel * 1000u / kBalloonScrollMaxPxPerSec;
    const std::uint32_t max_ms = travel * 1000u / kBalloonScrollMinPxPerSec;

    std::uint32_t edge_ms = kBalloonEdgeHoldMs;  // 動き出し前 / 動いた後の静止
    std::uint32_t move_ms = default_ms;
    if (hold_ms > 0) {
        // 指定時間の中で、前後の静止 (各 1/5、最大 1 秒) を除いた分を動かす。
        edge_ms = std::min(kBalloonEdgeHoldMs, hold_ms / 5);
        const std::uint32_t avail = hold_ms > 2 * edge_ms ? hold_ms - 2 * edge_ms : 0;
        move_ms = std::clamp(avail, min_ms, max_ms);
    }

    BalloonScroll s;
    if (elapsed_ms > edge_ms) {
        const std::uint32_t t = elapsed_ms - edge_ms;
        s.offset_px = t >= move_ms ? static_cast<std::int32_t>(travel)
                                   : static_cast<std::int32_t>(static_cast<std::uint64_t>(travel) * t / move_ms);
    }
    s.done = elapsed_ms >= std::max(hold_ms, edge_ms + move_ms + edge_ms);
    return s;
}

} // namespace stackchan::avatar::internal
