// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 吹き出しのスクロール計算 (balloon_layout.hpp) の検証。
#include <cstdio>

#include "../../balloon_layout.hpp"

using namespace stackchan::avatar::internal;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

}  // namespace

int main()
{
    constexpr std::int32_t kInner = 288;  // CoreS3 の吹き出し内側 (320 - 2*4 - 2*8)

    // 収まる文字列はスクロールしない (常に 0 = 左詰め)。既定では 3 秒見せて終了、
    // hold_ms があればそれまで。
    for (std::uint32_t t : {0u, 500u, 2999u}) {
        const auto s = compute_balloon_scroll(200, kInner, t, 0);
        CHECK(s.offset_px == 0 && !s.done);
    }
    CHECK(compute_balloon_scroll(200, kInner, 3000, 0).done);
    CHECK(compute_balloon_scroll(kInner, kInner, 0, 0).offset_px == 0);  // ちょうど収まる
    CHECK(!compute_balloon_scroll(200, kInner, 3500, 5000).done);
    CHECK(compute_balloon_scroll(200, kInner, 5000, 5000).done);

    // 溢れる文字列 (hold なし): 最初は左詰めで 1 秒止まり、60 px/s で溢れた分だけ動き、
    // 文末が右端に揃ったところで 1 秒止まって終了。ループしない。
    {
        const std::int32_t text_w = kInner + 300;  // 溢れは 300 px → 5 秒で動く
        CHECK(compute_balloon_scroll(text_w, kInner, 0, 0).offset_px == 0);      // 文頭が見えている
        CHECK(compute_balloon_scroll(text_w, kInner, 1000, 0).offset_px == 0);   // まだ動かない
        CHECK(compute_balloon_scroll(text_w, kInner, 2000, 0).offset_px == 60);  // 1 秒動いた = 60 px
        CHECK(compute_balloon_scroll(text_w, kInner, 6000, 0).offset_px == 300); // 文末が右端
        CHECK(compute_balloon_scroll(text_w, kInner, 60000, 0).offset_px == 300);  // それ以上は動かない
        CHECK(!compute_balloon_scroll(text_w, kInner, 6999, 0).done);
        CHECK(compute_balloon_scroll(text_w, kInner, 7000, 0).done);  // 1 + 5 + 1 秒
        // 単調増加 (途中で戻らない)。
        std::int32_t prev = 0;
        for (std::uint32_t t = 0; t <= 8000; t += 50) {
            const auto s = compute_balloon_scroll(text_w, kInner, t, 0);
            CHECK(s.offset_px >= prev && s.offset_px >= 0 && s.offset_px <= 300);
            prev = s.offset_px;
        }
    }

    // 溢れる文字列 (hold あり): その時間内に文末まで動く。前後の静止は 1/5 ずつ。
    {
        const std::int32_t text_w = kInner + 100;  // 溢れ 100 px
        const std::uint32_t hold = 3000;           // 前後 600 ms、動くのは 1800 ms (≈ 55 px/s)
        CHECK(compute_balloon_scroll(text_w, kInner, 0, hold).offset_px == 0);
        CHECK(compute_balloon_scroll(text_w, kInner, 600, hold).offset_px == 0);
        CHECK(compute_balloon_scroll(text_w, kInner, 2400, hold).offset_px == 100);  // 動き終わり
        CHECK(compute_balloon_scroll(text_w, kInner, hold - 1, hold).offset_px == 100);
        CHECK(!compute_balloon_scroll(text_w, kInner, hold - 1, hold).done);
        CHECK(compute_balloon_scroll(text_w, kInner, hold, hold).done);
    }
    // hold が短くて間に合わないときは最大速度 (120 px/s) で動く (それでも hold 内には終わらない)。
    {
        const std::int32_t text_w = kInner + 600;  // 溢れ 600 px → 最速 120 px/s でも 5 秒
        // hold=2000 → 前後の静止は各 400 ms。動き出して 800 ms 後 (elapsed=1200) は 120*0.8=96 px。
        CHECK(compute_balloon_scroll(text_w, kInner, 1200, 2000).offset_px == 96);
        CHECK(compute_balloon_scroll(text_w, kInner, 2000, 2000).offset_px == 192);  // 動き出して 1600 ms = 120*1.6。hold を過ぎても動き続ける
        CHECK(!compute_balloon_scroll(text_w, kInner, 2000, 2000).done);
        CHECK(compute_balloon_scroll(text_w, kInner, 5400, 2000).offset_px == 600);
        CHECK(!compute_balloon_scroll(text_w, kInner, 5799, 2000).done);
        CHECK(compute_balloon_scroll(text_w, kInner, 5800, 2000).done);  // 0.4 + 5 + 0.4 秒
    }
    // hold が長いときは最低速度 (30 px/s) まで落とす (それ以上は遅くしない)。
    {
        const std::int32_t text_w = kInner + 60;  // 溢れ 60 px → 最長 2 秒
        // hold = 30 秒: 前後は各 1 秒 (上限)、動くのは 2 秒 (30 px/s)、あとは静止のまま hold まで。
        CHECK(compute_balloon_scroll(text_w, kInner, 1000, 30000).offset_px == 0);
        CHECK(compute_balloon_scroll(text_w, kInner, 3000, 30000).offset_px == 60);
        CHECK(!compute_balloon_scroll(text_w, kInner, 29999, 30000).done);
        CHECK(compute_balloon_scroll(text_w, kInner, 30000, 30000).done);
    }

    if (g_failures == 0) std::puts("test_balloon_layout: all passed");
    return g_failures == 0 ? 0 : 1;
}
