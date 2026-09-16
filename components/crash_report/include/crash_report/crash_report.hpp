// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <string>

#include <esp_err.h>

namespace stackchan::crash_report {

// espcoredump を (再) 初期化する。IDF はシステム起動時にも初期化するが、その
// 時点では coredump 領域 (exttab の子) が未登録なので、flash_layout::init() の
// 後にもう一度呼ぶ。CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH でなければ no-op。
void init();

// 保存済みダンプがあるか。
bool has_dump();

// 前回パニックの要約を INFO/ERROR ログに出す (無ければ 1 行だけ)。
void log_boot_summary();

// {"present":bool, "reset_reason":"...", "task":"...", "pc":"0x...",
//  "exc_cause":n, "exc_vaddr":"0x...", "bt":["0x..",...], "bt_corrupted":bool,
//  "elf_sha256":"...."}
std::string summary_json();

// ダンプ領域を消す。
esp_err_t clear();

} // namespace stackchan::crash_report
