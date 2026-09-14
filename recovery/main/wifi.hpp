// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <string>

namespace stackchan::recovery::wifi {

// Wi-Fi STA を開始する。ssid が空なら driver だけ初期化して接続しない。
// 接続は非同期。切断時は即時再接続を試みる (Recovery に AP モードは無い)。
void start(const std::string& ssid, const std::string& password);

// IP アドレスを取得済みなら true。
bool connected();

} // namespace stackchan::recovery::wifi
