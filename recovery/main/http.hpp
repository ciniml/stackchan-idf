// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <string>

namespace stackchan::recovery::http {

// port 80 で Main の設定ページと同じ OTA ルートだけを公開する:
//   GET  /api/status        {"mode":"recovery","version":…,"wifi":bool}
//   GET  /api/ota/status
//   POST /api/ota/control   {"op":"begin","size":N} / {"op":"end"} / {"op":"abort"}
//   POST /api/ota/data      raw chunk (≤ 4 KiB)
//   POST /api/ota/release   {"tag":"vX.Y.Z"} → GitHub Pages から取得
// auth_password が空でなければ Basic 認証 (ユーザ名は任意、パスワードのみ照合)。
void start(const std::string& auth_password, std::uint8_t board_kind);

} // namespace stackchan::recovery::http
