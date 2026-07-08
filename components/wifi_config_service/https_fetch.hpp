// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// GitHub Pages (custom domain www.fugafuga.org) 越しの HTTPS GET を扱う共有
// ヘルパ。ストリーミング モード (esp_http_client_open + fetch_headers + read)
// は 30x を自動追従しないため、Location チェーンを手で辿って https へ
// アップグレードしながら最終ステータスを返す。release_ota (ファーム OTA) と
// voice_fetch (HMM ボイス取得) の両方が使う。

#pragma once

#include <cstdint>
#include <optional>

#include <esp_http_client.h>

namespace stackchan::wifi_config::https {

// Pages のカスタム ドメイン リダイレクト (301 → www.fugafuga.org) を手で辿り、
// 各ホップで http:// Location を https:// にアップグレードしてから再接続する。
// リダイレクトでない最終レスポンスの HTTP ステータスを返す。open / fetch 失敗、
// ホップ数超過、http(s) 以外のスキームへのリダイレクトは std::nullopt。
// content_length_out には最終レスポンスの Content-Length を書く。
std::optional<int> open_follow_redirects(esp_http_client_handle_t client,
                                         std::int64_t& content_length_out);

}  // namespace stackchan::wifi_config::https
