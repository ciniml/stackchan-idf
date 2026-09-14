// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// ADR-001: Main / Recovery から拡張パーティションテーブルと bootctl を扱う
// 共通 API。esp_ota_get_next_update_partition() / esp_ota_set_boot_partition()
// には依存しない (ADR-001「互換性と整合性」)。

#pragma once

#include <cstdint>
#include <string_view>

#include <esp_partition.h>
#include <tl/expected.hpp>

#include "flash_layout/format.h"

namespace stackchan::flash_layout {

enum class Error {
    NoExttabPartition,  // 標準テーブルに "exttab" (data/0x41) が無い
    NoBootctlPartition, // 標準テーブルに "bootctl" (data/0x40) が無い
    ExttabInvalid,      // A/B とも検証に失敗
    BootctlInvalid,     // A/B とも無効 (read_bootctl のみ。書き込み系は無効でも初期化して書く)
    FlashRead,
    FlashWrite,
    RegisterFailed,     // esp_partition_register_external が失敗 (重複など)
    TagTooLong,
    NotInitialized,
};

// 標準テーブル上の exttab / bootctl エントリのサブタイプ (partitions*.csv と一致させる)。
constexpr esp_partition_subtype_t kBootctlSubtype = static_cast<esp_partition_subtype_t>(0x40);
constexpr esp_partition_subtype_t kExttabSubtype = static_cast<esp_partition_subtype_t>(0x41);
// 拡張テーブルの RAW 領域を登録するときのサブタイプ。
constexpr esp_partition_subtype_t kRawSubtype = static_cast<esp_partition_subtype_t>(0x42);

struct Info {
    std::uint32_t generation;
    int active_copy;  // 0 = A, 1 = B
    std::uint32_t reserved_offset;
    std::uint32_t reserved_size;
    std::uint16_t entry_count;
};

// exttab の A/B を読んで検証し、採用した側の子領域を esp_partition に登録する。
// 2 回目以降の呼び出しは前回の結果を返す (登録は 1 回だけ)。
// Main は起動直後 (storage / voice / model を触る前) に、Recovery は書き込み
// 先を決める前に呼ぶ。
tl::expected<Info, Error> init();

// 登録済みの子領域をラベルで引く。init() 前、または該当無しなら nullptr。
const esp_partition_t* find(std::string_view label);

// --- bootctl -----------------------------------------------------------------

// 現在有効な bootctl を返す。
tl::expected<fl_bootctl_t, Error> read_bootctl();

// Main: 起動確認。target=Main, pending=0, attempts=0 で書き込む。
// 主要サブシステムの初期化が済んでから呼ぶ (ADR-001「起動先選択とロールバック」)。
tl::expected<void, Error> confirm_boot();

// Main: 更新開始。target=Recovery, request_tag=tag を書き込む。呼び出し側が
// その後に esp_restart() する。tag が空なら Recovery は待ち受けのみ。
tl::expected<void, Error> request_recovery(std::string_view tag);

// Recovery: Main の書き込み完了後。target=Main, pending=1, attempts=0,
// request_tag を消して書き込む。次回起動から試行カウントが始まる。
tl::expected<void, Error> arm_main();

// Recovery: 受信開始前に失敗し、旧 Main が無傷のとき。target=Main, pending=0
// (旧 Main は確認済みなので試行カウント無し)、request_tag を消す。
tl::expected<void, Error> return_to_main();

// Recovery: request_tag だけを消す (自動取得を消費したとき)。target は据え置き。
tl::expected<void, Error> clear_request_tag();

const char* error_name(Error e);

} // namespace stackchan::flash_layout
