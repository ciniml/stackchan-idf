// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <esp_err.h>
#include <esp_partition.h>

namespace stackchan::config::ota {

// JSON command set served by the OtaControl GATT characteristic:
//   {"op":"begin","size":<bytes>}   open the next OTA partition
//   {"op":"end"}                    finalise + set_boot + schedule reboot
//   {"op":"abort"}                  cancel an in-progress transfer
// Returns a JSON document describing the outcome (always non-empty).
std::string handle_control_command(const std::string& json);

// Append a chunk of firmware bytes (already decrypted from the BLE session)
// to the in-progress OTA partition. Returns the JSON status snapshot or an
// error document. Calling without an active begin() returns an error.
std::string handle_data_chunk(std::span<const std::uint8_t> data);

// Current state — also what an OtaControl READ returns.
//   {"state":"idle"|"receiving"|"done"|"failed",
//    "received":<bytes>,"total":<bytes>,"error":"<msg>"}
std::string status_json();

// Drop any in-progress transfer (esp_ota_abort). Call on BLE disconnect so
// a half-finished image can never be marked bootable.
void abort_update();

// --- ADR-001 レイアウト向けの差し替え点 ---------------------------------------
// どれも未設定なら従来 (otadata + 待機側 OTA スロット) の動作。

// 書き込み先。nullptr = esp_ota_get_next_update_partition() (従来)。
// Recovery は flash_layout::find("main") を渡す。
void set_target_partition(const esp_partition_t* part);

// 書き込み・検証完了後に起動先を切り替える処理。nullptr =
// esp_ota_set_boot_partition (従来)。Recovery は flash_layout::arm_main を渡す。
using FinalizeFn = esp_err_t (*)(const esp_partition_t* written);
void set_finalize_hook(FinalizeFn fn);

// Main が ADR-001 レイアウトで動くとき: begin を受けたら自分では書かず、
// Recovery への引き継ぎ (bootctl 書き込み + 再起動予約) を行う。
// 戻り値 true = 引き継いだ (呼び出し元は "rebooting to recovery" を返す)。
using HandoffFn = bool (*)();
void set_handoff_hook(HandoffFn fn);

// Project name an incoming image must carry in its esp_app_desc_t. Default
// (empty) = the running app's own project_name, which is what Main wants.
// Recovery (project "stackchan_recovery") sets this to "stackchan_idf" so it
// accepts Main images while still rejecting foreign firmware.
void set_expected_project_name(std::string_view name);

} // namespace stackchan::config::ota
