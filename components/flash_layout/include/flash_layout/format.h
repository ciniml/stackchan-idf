// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// ADR-001 拡張パーティションテーブル (exttab) と起動制御セクタ (bootctl) の
// バイナリ形式と、その検証・選択ロジック。
//
// この header と format.c は ESP-IDF に依存しない純粋な C で、次の 3 箇所で
// 同じソースをコンパイルする:
//   - カスタム 2 段目ブートローダー (bootloader_components/main)
//   - Main / Recovery アプリ (components/flash_layout)
//   - ホスト ユニットテスト (components/flash_layout/test/host)
// そのため malloc / printf / IDF API は使わない。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- 共通 ------------------------------------------------------------------

#define FL_SECTOR_SIZE 0x1000u
#define FL_APP_ALIGN 0x10000u

// 固定領域の位置 (ADR-001「容量方針」)。標準テーブルにも同じ値で載せるが、
// Main の開発用テーブル (partitions_main_*.csv) は IDF の check_sizes と
// app 書き込みオフセットの都合で recovery を載せない。その場合ブートローダーと
// flash_layout はこの既定値を使う。
#define FL_RECOVERY_OFFSET 0x10000u
#define FL_RECOVERY_SIZE 0x180000u
#define FL_BOOTCTL_OFFSET 0xd000u
#define FL_EXTTAB_OFFSET 0x190000u

// CRC-32 (IEEE 802.3、反転あり、esp_rom_crc32_le と同じ多項式・初期値扱い)。
// crc の初期値には前回の戻り値を渡して連結できる。最初は 0。
uint32_t fl_crc32(uint32_t crc, const uint8_t* buf, size_t len);

// --- 拡張パーティションテーブル (exttab) -------------------------------------
//
// 1 セクタ (4 KiB) に header + entries を置く。A/B の 2 セクタを持ち、CRC が
// 正しく generation が大きい側を採用する。

#define FL_EXTTAB_MAGIC 0x54584353u  // 'S','C','X','T' (little-endian)
#define FL_EXTTAB_FORMAT_VERSION 1u
#define FL_EXTTAB_MAX_ENTRIES 32u
#define FL_LABEL_LEN 16u

enum fl_kind {
    FL_KIND_APP = 1,       // 起動可能なアプリイメージ (Main)
    FL_KIND_RAW = 2,       // 生データ (先頭に用途ごとのヘッダ: HMM 音声など)
    FL_KIND_SPIFFS = 3,    // SPIFFS
    FL_KIND_LITTLEFS = 4,  // LittleFS
    FL_KIND_COREDUMP = 5,  // ESP-IDF espcoredump (data/coredump)。パニック時の保存先
};

// entry.flags
#define FL_FLAG_REQUIRED 0x0001u  // 読み手が kind を知らなければテーブル全体を拒否する

typedef struct __attribute__((packed)) {
    uint32_t magic;            // FL_EXTTAB_MAGIC
    uint16_t format_version;   // FL_EXTTAB_FORMAT_VERSION
    uint16_t entry_count;      // 1..FL_EXTTAB_MAX_ENTRIES
    uint32_t generation;       // 書き換えごとに +1。A/B の新旧判定
    uint32_t reserved_offset;  // 予約範囲の先頭 (フラッシュ絶対、FL_APP_ALIGN 境界)
    uint32_t reserved_size;    // 予約範囲の大きさ
    uint32_t crc32;            // header の crc32 より前の 20 バイト + entries 全体
} fl_exttab_header_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;          // reserved_offset からの相対
    uint32_t size;
    char label[FL_LABEL_LEN]; // NUL 終端 (最長 15 文字)
    uint8_t kind;             // enum fl_kind
    uint8_t format_version;   // 領域内データ形式のバージョン (用途ごと)
    uint16_t flags;           // FL_FLAG_*
    uint32_t reserved;        // 0
} fl_exttab_entry_t;

#define FL_EXTTAB_HEADER_CRC_LEN (sizeof(fl_exttab_header_t) - sizeof(uint32_t))

// 検証済みテーブルへの view。sector は呼び出し側が保持するバッファを指す。
typedef struct {
    const fl_exttab_header_t* header;
    const fl_exttab_entry_t* entries;
    uint16_t entry_count;
} fl_exttab_view_t;

enum fl_status {
    FL_OK = 0,
    FL_ERR_MAGIC,
    FL_ERR_VERSION,
    FL_ERR_COUNT,
    FL_ERR_CRC,
    FL_ERR_RANGE,      // 予約範囲がフラッシュに収まらない / アラインメント違反
    FL_ERR_ENTRY,      // エントリの範囲外・アラインメント違反・ラベル不正
    FL_ERR_OVERLAP,    // エントリ同士の重複
    FL_ERR_KIND,       // 未知の必須形式
    FL_ERR_NO_APP,     // FL_KIND_APP のエントリが無い
};

// sector (少なくとも FL_SECTOR_SIZE バイト) を検証し、成功なら out に view を返す。
// flash_size はフラッシュ全体の大きさ (予約範囲の上限チェックに使う)。
enum fl_status fl_exttab_validate(const uint8_t* sector, size_t sector_len, uint32_t flash_size,
                                  fl_exttab_view_t* out);

// A/B の検証結果から採用する側を返す。0 = A、1 = B、-1 = 両方無効。
// 両方有効なら generation が大きい側 (同じなら A)。
int fl_exttab_select(enum fl_status status_a, const fl_exttab_view_t* a, enum fl_status status_b,
                     const fl_exttab_view_t* b);

// label で検索。見つからなければ NULL。
const fl_exttab_entry_t* fl_exttab_find(const fl_exttab_view_t* view, const char* label);

// header + entries の CRC を計算する (書き込み側用)。
uint32_t fl_exttab_compute_crc(const fl_exttab_header_t* header, const fl_exttab_entry_t* entries,
                               uint16_t entry_count);

// --- 起動制御 (bootctl) ------------------------------------------------------
//
// otadata と同じ流儀: A/B 各 1 セクタ、消去後書き込み、seq が大きい側が有効。

#define FL_BOOTCTL_MAGIC 0x4C434253u  // 'S','B','C','L'
#define FL_BOOTCTL_FORMAT_VERSION 1u
#define FL_BOOTCTL_TAG_LEN 32u
#define FL_BOOTCTL_DEFAULT_MAX_ATTEMPTS 3u

enum fl_target {
    FL_TARGET_RECOVERY = 0,
    FL_TARGET_MAIN = 1,
};

typedef struct __attribute__((packed)) {
    uint32_t magic;           // FL_BOOTCTL_MAGIC
    uint16_t format_version;  // FL_BOOTCTL_FORMAT_VERSION
    uint16_t reserved0;       // 0
    uint32_t seq;             // 書き込みごとに +1
    uint8_t target;           // enum fl_target
    uint8_t attempts;         // pending 中の Main 起動試行回数
    uint8_t max_attempts;     // これに達したら Recovery
    uint8_t pending;          // 1 = Main が起動確認をまだ返していない
    char request_tag[FL_BOOTCTL_TAG_LEN];  // Recovery に自動取得させるタグ (空 = 無し)
    uint32_t crc32;           // crc32 より前の全バイト
} fl_bootctl_t;

#define FL_BOOTCTL_CRC_LEN (sizeof(fl_bootctl_t) - sizeof(uint32_t))

// 1 セクタ分のバッファ先頭にある bootctl を検証する。
bool fl_bootctl_valid(const fl_bootctl_t* b);

// A/B から採用する側を返す。0 = A、1 = B、-1 = 両方無効。seq が大きい側 (同じなら A)。
int fl_bootctl_select(const fl_bootctl_t* a, const fl_bootctl_t* b);

// 書き込み側用: crc32 を計算して設定する。
void fl_bootctl_finalize(fl_bootctl_t* b);

enum fl_boot_decision {
    FL_BOOT_RECOVERY = 0,
    FL_BOOT_MAIN = 1,
};

// ブートローダーの起動判断。cur は採用した bootctl (無効なら NULL)。
// 戻り値が FL_BOOT_MAIN で *write_next が true なら、next を (seq+1 で) 古い側の
// セクタに書き込んでから Main を起動する (pending 中の試行カウント)。
enum fl_boot_decision fl_boot_decide(const fl_bootctl_t* cur, fl_bootctl_t* next, bool* write_next);

#ifdef __cplusplus
}
#endif
