// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// M5Stack 公式 Stack-chan (github.com/m5stack/StackChan) 互換の ESP-NOW リモコン
// 受信。8 バイト固定・リトルエンディアンのポーズ パケット
//   [0]=target-id u8, [1..2]=yaw i16, [3..4]=pitch i16, [5..6]=speed i16, [7]=laser u8
// を espressif/esp-now コンポーネント (公式と同一トランスポート) で受け、
// target-id が 0(broadcast) か自機 receiver_id に一致する時だけハンドラを呼ぶ。
//
// 本コンポーネントは main の SharedState/servo を知らない (依存を持たない)。
// 受信ポーズは PoseHandler コールバックで呼び出し側 (main) に渡し、そこで
// サーボ目標角へ写像する。WiFi は STA + 固定チャネル (AP 非接続) で初期化する
// ため、通常の wifi_start (AP 接続) とは排他 — 呼び出し側が ESP-NOW モードで
// wifi_start を呼ばないこと。詳細は docs/espnow-m5stackchan-protocol-research.md。

#pragma once

#include <cstdint>
#include <functional>

#include <esp_err.h>
#include <tl/expected.hpp>

namespace stackchan::espnow {

// 受信した (フィルタ通過後の) 1 ポーズ。角度単位は on-wire の生値 (公式は 0.1 度
// 想定: yaw ±1280 = ±128.0°, pitch 0..900 = 0..90.0°)。写像は呼び出し側の責務。
struct Pose {
    std::uint8_t target_id = 0;
    std::int16_t yaw = 0;
    std::int16_t pitch = 0;
    std::int16_t speed = 0;
    std::uint8_t laser = 0;
    std::int8_t rssi = 0;  // 受信 RSSI [dBm] (診断用)
};

using PoseHandler = std::function<void(const Pose&)>;

struct ReceiverConfig {
    std::uint8_t channel = 1;      // 1..13 (範囲外は [1,13] にクランプ)
    std::uint8_t receiver_id = 1;  // 自機 ID。target-id が 0 か この値の時だけ採用
};

// WiFi(固定チャネル) + ESP-NOW を初期化し、受信を開始する。ハンドラは
// ESP-NOW の受信コンテキスト (専用タスク) から呼ばれるので、軽量・スレッド
// セーフに保つこと (SharedState の atomic store 程度)。二重初期化 (esp_netif /
// event loop) は ESP_ERR_INVALID_STATE を許容する。成功で戻り、以後は
// バックグラウンドで受信し続ける。
tl::expected<void, esp_err_t> start(const ReceiverConfig& cfg, PoseHandler on_pose);

}  // namespace stackchan::espnow
