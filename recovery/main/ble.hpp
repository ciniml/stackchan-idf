// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <string>

namespace stackchan::recovery::ble {

// NimBLE を起動し、Main と同じ設定サービス UUID (e3f0a000-…) で
// KeyExchange / OtaControl / OtaData の 3 characteristic だけを公開する。
// 既存の tools/ble-cli や Web 設定ページの OTA フローがそのまま使える。
//   device_name   : 空なら "Stackchan-XXXXXX" (Wi-Fi STA MAC 下位 3 バイト)
//   auth_password : 空でなければ SHA-256 を HKDF salt にする (Main と同じ認証ゲート)
void start(const std::string& device_name, const std::string& auth_password);

bool connected();

} // namespace stackchan::recovery::ble
