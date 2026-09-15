// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS の起動時ベンチマーク (CONFIG_STACKCHAN_SANO_BENCH)。BLE / Wi-Fi を始める前、
// 内部 RAM が最も空いている時点で重みをロードし固定文を合成して RTF を記録する。
// 「PSRAM 上の arena が遅さの原因か」を確定するための技術検証。
#pragma once

namespace stackchan::app::sano_bench {

// 重み未登録 / sanoTTS 無効なら何もしない。数秒ブロックする。
void run();

}  // namespace stackchan::app::sano_bench
