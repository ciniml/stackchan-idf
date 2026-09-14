// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// exttab / bootctl の検証・選択ロジック。ESP-IDF 非依存 (format.h 参照)。

#include "flash_layout/format.h"

#include <string.h>

// --- CRC-32 ---------------------------------------------------------------
// テーブル無しのビット単位実装。4 KiB を毎起動 1 回計算する程度なので十分速い。
// esp_rom_crc32_le と同じ結果になる (反転入力/出力、多項式 0xEDB88320)。
uint32_t fl_crc32(uint32_t crc, const uint8_t* buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// --- exttab ---------------------------------------------------------------

uint32_t fl_exttab_compute_crc(const fl_exttab_header_t* header, const fl_exttab_entry_t* entries,
                               uint16_t entry_count)
{
    uint32_t crc = fl_crc32(0, (const uint8_t*)header, FL_EXTTAB_HEADER_CRC_LEN);
    return fl_crc32(crc, (const uint8_t*)entries, (size_t)entry_count * sizeof(fl_exttab_entry_t));
}

static bool label_ok(const char* label)
{
    if (label[0] == '\0') return false;
    for (size_t i = 0; i < FL_LABEL_LEN; ++i) {
        if (label[i] == '\0') return true;
    }
    return false;  // NUL 終端なし
}

static bool kind_known(uint8_t kind)
{
    return kind == FL_KIND_APP || kind == FL_KIND_RAW || kind == FL_KIND_SPIFFS || kind == FL_KIND_LITTLEFS;
}

enum fl_status fl_exttab_validate(const uint8_t* sector, size_t sector_len, uint32_t flash_size,
                                  fl_exttab_view_t* out)
{
    if (sector_len < sizeof(fl_exttab_header_t)) return FL_ERR_COUNT;
    const fl_exttab_header_t* h = (const fl_exttab_header_t*)sector;
    if (h->magic != FL_EXTTAB_MAGIC) return FL_ERR_MAGIC;
    if (h->format_version != FL_EXTTAB_FORMAT_VERSION) return FL_ERR_VERSION;
    if (h->entry_count == 0 || h->entry_count > FL_EXTTAB_MAX_ENTRIES) return FL_ERR_COUNT;
    const size_t need = sizeof(fl_exttab_header_t) + (size_t)h->entry_count * sizeof(fl_exttab_entry_t);
    if (need > sector_len) return FL_ERR_COUNT;
    const fl_exttab_entry_t* e = (const fl_exttab_entry_t*)(sector + sizeof(fl_exttab_header_t));
    if (fl_exttab_compute_crc(h, e, h->entry_count) != h->crc32) return FL_ERR_CRC;

    if ((h->reserved_offset % FL_APP_ALIGN) != 0) return FL_ERR_RANGE;
    if (h->reserved_size == 0) return FL_ERR_RANGE;
    if ((uint64_t)h->reserved_offset + h->reserved_size > flash_size) return FL_ERR_RANGE;

    bool has_app = false;
    for (uint16_t i = 0; i < h->entry_count; ++i) {
        const fl_exttab_entry_t* a = &e[i];
        if (a->size == 0) return FL_ERR_ENTRY;
        if ((uint64_t)a->offset + a->size > h->reserved_size) return FL_ERR_ENTRY;
        const uint32_t align = (a->kind == FL_KIND_APP) ? FL_APP_ALIGN : FL_SECTOR_SIZE;
        if ((a->offset % align) != 0 || (a->size % FL_SECTOR_SIZE) != 0) return FL_ERR_ENTRY;
        if (!label_ok(a->label)) return FL_ERR_ENTRY;
        if (!kind_known(a->kind)) {
            if (a->flags & FL_FLAG_REQUIRED) return FL_ERR_KIND;
        } else if (a->kind == FL_KIND_APP) {
            has_app = true;
        }
        for (uint16_t j = 0; j < i; ++j) {
            const fl_exttab_entry_t* b = &e[j];
            if (a->offset < b->offset + b->size && b->offset < a->offset + a->size) return FL_ERR_OVERLAP;
            if (strncmp(a->label, b->label, FL_LABEL_LEN) == 0) return FL_ERR_OVERLAP;
        }
    }
    if (!has_app) return FL_ERR_NO_APP;

    if (out != NULL) {
        out->header = h;
        out->entries = e;
        out->entry_count = h->entry_count;
    }
    return FL_OK;
}

int fl_exttab_select(enum fl_status status_a, const fl_exttab_view_t* a, enum fl_status status_b,
                     const fl_exttab_view_t* b)
{
    const bool va = (status_a == FL_OK);
    const bool vb = (status_b == FL_OK);
    if (va && vb) return (b->header->generation > a->header->generation) ? 1 : 0;
    if (va) return 0;
    if (vb) return 1;
    return -1;
}

const fl_exttab_entry_t* fl_exttab_find(const fl_exttab_view_t* view, const char* label)
{
    for (uint16_t i = 0; i < view->entry_count; ++i) {
        if (strncmp(view->entries[i].label, label, FL_LABEL_LEN) == 0) return &view->entries[i];
    }
    return NULL;
}

// --- bootctl --------------------------------------------------------------

bool fl_bootctl_valid(const fl_bootctl_t* b)
{
    if (b->magic != FL_BOOTCTL_MAGIC) return false;
    if (b->format_version != FL_BOOTCTL_FORMAT_VERSION) return false;
    if (b->target != FL_TARGET_RECOVERY && b->target != FL_TARGET_MAIN) return false;
    if (b->pending > 1) return false;
    if (b->request_tag[FL_BOOTCTL_TAG_LEN - 1] != '\0') return false;
    return fl_crc32(0, (const uint8_t*)b, FL_BOOTCTL_CRC_LEN) == b->crc32;
}

int fl_bootctl_select(const fl_bootctl_t* a, const fl_bootctl_t* b)
{
    const bool va = fl_bootctl_valid(a);
    const bool vb = fl_bootctl_valid(b);
    if (va && vb) return (b->seq > a->seq) ? 1 : 0;
    if (va) return 0;
    if (vb) return 1;
    return -1;
}

void fl_bootctl_finalize(fl_bootctl_t* b)
{
    b->magic = FL_BOOTCTL_MAGIC;
    b->format_version = FL_BOOTCTL_FORMAT_VERSION;
    b->reserved0 = 0;
    b->request_tag[FL_BOOTCTL_TAG_LEN - 1] = '\0';
    b->crc32 = fl_crc32(0, (const uint8_t*)b, FL_BOOTCTL_CRC_LEN);
}

enum fl_boot_decision fl_boot_decide(const fl_bootctl_t* cur, fl_bootctl_t* next, bool* write_next)
{
    *write_next = false;
    if (cur == NULL || cur->target != FL_TARGET_MAIN) return FL_BOOT_RECOVERY;
    if (!cur->pending) return FL_BOOT_MAIN;
    if (cur->attempts >= cur->max_attempts) return FL_BOOT_RECOVERY;
    *next = *cur;
    next->seq = cur->seq + 1;
    next->attempts = cur->attempts + 1;
    fl_bootctl_finalize(next);
    *write_next = true;
    return FL_BOOT_MAIN;
}
