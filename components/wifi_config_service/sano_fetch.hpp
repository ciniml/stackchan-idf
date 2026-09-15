// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS-jp の重み blob を公式 GitHub Releases から機体が直接ダウンロードして
// インストールする。重みは非 MIT (上流 LICENSE-MODEL.md) なので本プロジェクトは
// 再配布せず、利用者の機体が公式配布物を取りに行く。
//
// SSRF 回避のため URL は固定: 呼び出し側が渡すのは検証済みのリリース タグと
// ファイル名 (英数 . _ -、".." 不可) だけで、機体が
//   https://github.com/ayutaz/sanoTTS-jp/releases/download/<tag>/<file>
// を組む (objects.githubusercontent.com へのリダイレクトを追従する)。
// voice_fetch と同じく httpd タスク上で同期実行する。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace stackchan::wifi_config::sano_fetch {

using InstallFn = std::function<const char*(const std::uint8_t* data, std::size_t len)>;

// 成功で nullptr、失敗で静的エラー文字列。数〜数十秒ブロックする。
const char* fetch_and_install(const std::string& release_tag, const std::string& file_name,
                              const InstallFn& install);

}  // namespace stackchan::wifi_config::sano_fetch
