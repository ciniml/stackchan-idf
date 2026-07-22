// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

namespace stackchan::app {

// ESP-NOW PoC を起動する(無限ループ・戻らない)。CONFIG_STACKCHAN_ESPNOW_POC
// 無効時は no-op。WiFi は自前で STA + 固定チャネル初期化する(通常起動の WiFi は
// 使わない前提 = 専用モード)。
void espnow_poc_run();

}  // namespace stackchan::app
