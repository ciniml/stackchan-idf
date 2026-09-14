// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// ADR-001 カスタム 2 段目ブートローダー。
//
// IDF 標準の bootloader_start.c との違いは起動先の決め方だけ:
//   1. 標準パーティションテーブルを自前で走査し、"recovery" / "bootctl" /
//      "exttab" をラベルで探す。recovery が無ければ (Main 開発用テーブル)
//      format.h の固定オフセットを使う。
//   2. bootctl A/B を読んで fl_boot_decide() で Main / Recovery を決める。
//      pending 中の Main 起動なら試行カウンタを進めて書き戻す。
//   3. Main なら exttab A/B を検証して "main" エントリの位置を取り、IDF の
//      bootloader_utility_load_boot_image() に ota[0] として渡す。イメージが
//      壊れていれば同関数が factory (= recovery) にフォールバックする。
//   4. それ以外は recovery を起動する。recovery すら無ければリセット。
//
// otadata / CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE は使わない。

#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_spiflash.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "bootloader_hooks.h"
#include "bootloader_flash_priv.h"
#include "esp_flash_partitions.h"

#include "flash_layout/format.h"

static const char* TAG = "boot";

// ラベル検索した固定領域。見つからなければ offset = 0。
typedef struct {
    esp_partition_pos_t recovery;
    esp_partition_pos_t bootctl;
    esp_partition_pos_t exttab;
} fixed_layout_t;

static bool load_fixed_layout(fixed_layout_t* out)
{
    memset(out, 0, sizeof(*out));
    const esp_partition_info_t* table =
        bootloader_mmap(ESP_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_MAX_LEN);
    if (table == NULL) {
        ESP_LOGE(TAG, "bootloader_mmap(0x%x) failed", ESP_PARTITION_TABLE_OFFSET);
        return false;
    }
    int num = 0;
    esp_err_t err = esp_partition_table_verify(table, true, &num);
    if (err != ESP_OK) {
        bootloader_munmap(table);
        return false;
    }
    for (int i = 0; i < num; ++i) {
        const esp_partition_info_t* p = &table[i];
        if (strncmp((const char*)p->label, "recovery", sizeof(p->label)) == 0) {
            out->recovery = p->pos;
        } else if (strncmp((const char*)p->label, "bootctl", sizeof(p->label)) == 0) {
            out->bootctl = p->pos;
        } else if (strncmp((const char*)p->label, "exttab", sizeof(p->label)) == 0) {
            out->exttab = p->pos;
        }
    }
    bootloader_munmap(table);
    if (out->recovery.offset == 0) {
        // Main の開発用テーブルには recovery が載らない。固定値を使う。
        out->recovery.offset = FL_RECOVERY_OFFSET;
        out->recovery.size = FL_RECOVERY_SIZE;
        ESP_LOGW(TAG, "no 'recovery' entry — using fixed 0x%08x", (unsigned)FL_RECOVERY_OFFSET);
    }
    return true;
}

// bootctl A/B を読み、採用した側を *cur にコピーして返す。無効なら false。
static bool read_bootctl(const esp_partition_pos_t* pos, fl_bootctl_t* cur, int* active)
{
    fl_bootctl_t a, b;
    if (bootloader_flash_read(pos->offset, &a, sizeof(a), false) != ESP_OK) return false;
    if (bootloader_flash_read(pos->offset + FL_SECTOR_SIZE, &b, sizeof(b), false) != ESP_OK) return false;
    *active = fl_bootctl_select(&a, &b);
    if (*active < 0) return false;
    *cur = (*active == 0) ? a : b;
    return true;
}

// pending 中の試行カウントを、現在有効でない側のセクタへ書く。
static void write_bootctl(const esp_partition_pos_t* pos, int active, fl_bootctl_t* next)
{
    const uint32_t addr = pos->offset + ((active == 0) ? FL_SECTOR_SIZE : 0);
    if (bootloader_flash_erase_sector(addr / FL_SECTOR_SIZE) != ESP_OK ||
        bootloader_flash_write(addr, next, sizeof(*next), false) != ESP_OK) {
        ESP_LOGE(TAG, "bootctl write failed at 0x%08x", (unsigned)addr);
    }
}

// exttab の片側を mmap して "main" の位置と世代を取り出す。
static bool read_exttab_copy(uint32_t addr, uint32_t flash_size, esp_partition_pos_t* main_pos,
                             uint32_t* generation)
{
    const uint8_t* sector = bootloader_mmap(addr, FL_SECTOR_SIZE);
    if (sector == NULL) return false;
    fl_exttab_view_t view;
    enum fl_status st = fl_exttab_validate(sector, FL_SECTOR_SIZE, flash_size, &view);
    bool ok = false;
    if (st == FL_OK) {
        const fl_exttab_entry_t* e = fl_exttab_find(&view, "main");
        if (e != NULL && e->kind == FL_KIND_APP) {
            main_pos->offset = view.header->reserved_offset + e->offset;
            main_pos->size = e->size;
            *generation = view.header->generation;
            ok = true;
        } else {
            ESP_LOGE(TAG, "exttab@0x%08x: no 'main' app entry", (unsigned)addr);
        }
    } else {
        ESP_LOGW(TAG, "exttab@0x%08x invalid (%d)", (unsigned)addr, (int)st);
    }
    bootloader_munmap(sector);
    return ok;
}

static bool find_main(const esp_partition_pos_t* exttab, esp_partition_pos_t* main_pos)
{
    const uint32_t flash_size = g_rom_flashchip.chip_size;
    esp_partition_pos_t pa = {0}, pb = {0};
    uint32_t ga = 0, gb = 0;
    const bool va = read_exttab_copy(exttab->offset, flash_size, &pa, &ga);
    const bool vb = read_exttab_copy(exttab->offset + FL_SECTOR_SIZE, flash_size, &pb, &gb);
    if (!va && !vb) return false;
    if (va && vb) {
        *main_pos = (gb > ga) ? pb : pa;
    } else {
        *main_pos = va ? pa : pb;
    }
    return true;
}

static __attribute__((noreturn)) void boot_recovery(const fixed_layout_t* fl)
{
    bootloader_state_t bs = {0};
    bs.factory = fl->recovery;
    ESP_LOGI(TAG, "booting recovery @0x%08x", (unsigned)fl->recovery.offset);
    bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
}

static __attribute__((noreturn)) void select_and_boot(void)
{
    fixed_layout_t fl;
    if (!load_fixed_layout(&fl)) {
        ESP_LOGE(TAG, "partition table unusable");
        bootloader_reset();
    }
    if (fl.bootctl.offset == 0 || fl.exttab.offset == 0) {
        ESP_LOGW(TAG, "no bootctl/exttab in partition table — legacy layout, booting recovery");
        boot_recovery(&fl);
    }

    fl_bootctl_t cur;
    int active = -1;
    const bool have_ctl = read_bootctl(&fl.bootctl, &cur, &active);
    fl_bootctl_t next;
    bool write_next = false;
    enum fl_boot_decision d = fl_boot_decide(have_ctl ? &cur : NULL, &next, &write_next);
    if (have_ctl) {
        ESP_LOGI(TAG, "bootctl seq=%u target=%s pending=%u attempts=%u/%u", (unsigned)cur.seq,
                 cur.target == FL_TARGET_MAIN ? "main" : "recovery", cur.pending, cur.attempts,
                 cur.max_attempts);
    } else {
        ESP_LOGW(TAG, "bootctl invalid/erased");
    }
    if (d != FL_BOOT_MAIN) {
        boot_recovery(&fl);
    }

    esp_partition_pos_t main_pos = {0};
    if (!find_main(&fl.exttab, &main_pos)) {
        ESP_LOGE(TAG, "exttab invalid — booting recovery");
        boot_recovery(&fl);
    }
    if (write_next) {
        write_bootctl(&fl.bootctl, active, &next);
        ESP_LOGI(TAG, "main boot attempt %u/%u", next.attempts, next.max_attempts);
    }

    // ota[0] = main、factory = recovery。main が壊れていれば IDF が factory へ
    // フォールバックする。
    bootloader_state_t bs = {0};
    bs.factory = fl.recovery;
    bs.ota[0] = main_pos;
    bs.app_count = 1;
    ESP_LOGI(TAG, "booting main @0x%08x (0x%x)", (unsigned)main_pos.offset, (unsigned)main_pos.size);
    bootloader_utility_load_boot_image(&bs, 0);
}

/*
 * ROM ブートローダーから呼ばれるエントリ。IDF 標準と同じ手順で HW を初期化し、
 * 起動先の選択だけを差し替える。
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_before_init) {
        bootloader_before_init();
    }
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }
    if (bootloader_after_init) {
        bootloader_after_init();
    }
    select_and_boot();
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent* __getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
