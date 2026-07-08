// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HMM ボイス (.htsvoice) を GitHub Pages からダウンロードして flash の voice
// パーティションへインストールする。
//
// 同期実行: 内部 RAM は steady-state で断片化していて (最大空きブロック ~10 KiB)
// mbedTLS ハンドシェイク + hts パースに要る 16 KiB 連続の内部スタックを実行時に
// 確保できない。そこでワーカー タスクを作らず、ボート時に確保済みの httpd タスク
// スタック (16 KiB 内部、start_http_server で設定) 上でダウンロード+インストール
// を同期で行う。呼び出し (POST /api/hmm-voice/fetch) はその間ブロックする。
//
// SSRF 回避のため URL は組み立て固定: 呼び出し側が渡すのは検証済みボイス ID
// (英数 . _ -、".." 不可) のみで、デバイスが
//   https://ciniml.github.io/stackchan-idf/voices/<id>.htsvoice
// を自分で組む。マニフェスト voices.json も同じホストから取得して UI に渡す。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace stackchan::wifi_config::voice_fetch {

// ボイスをインストールするコールバック。ダウンロードした .htsvoice の生バイトを
// 受け取り、成功で nullptr、失敗で静的エラー文字列 (= HmmVoiceSink と同型)。
using InstallFn = std::function<const char*(const std::uint8_t* data, std::size_t len)>;

// voices/<id>.htsvoice を同期ダウンロードして install_fn でインストールする。
// 成功で nullptr、失敗で静的エラー文字列 (HTTP レスポンスにそのまま使える)。
// 呼び出しスレッド (httpd タスク) を数〜数十秒ブロックする。
const char* fetch_and_install(const std::string& voice_id, const InstallFn& install);

// Pages の voices.json (配布ボイス一覧マニフェスト) を同期 HTTPS 取得して out に
// 書く。成功で true。
bool fetch_manifest(std::string& out);

}  // namespace stackchan::wifi_config::voice_fetch
