// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <flash_layout/format.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                              \
            ++g_failures;                                                                            \
        }                                                                                            \
    } while (0)

constexpr std::uint32_t kFlash16M = 0x1000000;
constexpr std::uint32_t kReserved = 0x1A0000;

using Sector = std::array<std::uint8_t, FL_SECTOR_SIZE>;

fl_exttab_entry_t entry(const char* label, std::uint32_t off, std::uint32_t size, std::uint8_t kind,
                        std::uint16_t flags = 0)
{
    fl_exttab_entry_t e{};
    e.offset = off;
    e.size = size;
    std::strncpy(e.label, label, FL_LABEL_LEN - 1);
    e.kind = kind;
    e.format_version = 1;
    e.flags = flags;
    return e;
}

// ADR-001 の 16MB 配置。
std::vector<fl_exttab_entry_t> layout16()
{
    return {
        entry("main", 0x0, 0x500000, FL_KIND_APP),
        entry("storage", 0x500000, 0x100000, FL_KIND_SPIFFS),
        entry("voice", 0x600000, 0x380000, FL_KIND_RAW),
        entry("model", 0x980000, 0x2E0000, FL_KIND_SPIFFS),
        entry("sanotts", 0xC60000, 0xA0000, FL_KIND_RAW),
        entry("bluescript", 0xD00000, 0x100000, FL_KIND_RAW),
    };
}

Sector make_sector(const std::vector<fl_exttab_entry_t>& entries, std::uint32_t generation = 1,
                   std::uint32_t reserved_offset = kReserved, std::uint32_t reserved_size = kFlash16M - kReserved)
{
    Sector s{};
    s.fill(0xff);
    fl_exttab_header_t h{};
    h.magic = FL_EXTTAB_MAGIC;
    h.format_version = FL_EXTTAB_FORMAT_VERSION;
    h.entry_count = static_cast<std::uint16_t>(entries.size());
    h.generation = generation;
    h.reserved_offset = reserved_offset;
    h.reserved_size = reserved_size;
    h.crc32 = fl_exttab_compute_crc(&h, entries.data(), h.entry_count);
    std::memcpy(s.data(), &h, sizeof(h));
    std::memcpy(s.data() + sizeof(h), entries.data(), entries.size() * sizeof(fl_exttab_entry_t));
    return s;
}

void test_crc32_known_vector()
{
    // CRC-32 of "123456789" = 0xCBF43926 (IEEE)。
    const char* v = "123456789";
    CHECK(fl_crc32(0, reinterpret_cast<const std::uint8_t*>(v), 9) == 0xCBF43926u);
    // 連結しても同じ。
    std::uint32_t c = fl_crc32(0, reinterpret_cast<const std::uint8_t*>(v), 4);
    c = fl_crc32(c, reinterpret_cast<const std::uint8_t*>(v + 4), 5);
    CHECK(c == 0xCBF43926u);
}

void test_exttab_valid_layout()
{
    Sector s = make_sector(layout16());
    fl_exttab_view_t v{};
    CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, &v) == FL_OK);
    CHECK(v.entry_count == 6);
    const fl_exttab_entry_t* m = fl_exttab_find(&v, "main");
    CHECK(m != nullptr && m->kind == FL_KIND_APP && m->size == 0x500000);
    CHECK(fl_exttab_find(&v, "nope") == nullptr);
    // 8MB のフラッシュには収まらない。
    CHECK(fl_exttab_validate(s.data(), s.size(), 0x800000, nullptr) == FL_ERR_RANGE);
}

void test_exttab_rejects()
{
    {
        Sector s = make_sector(layout16());
        s[0] ^= 0xff;
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_MAGIC);
    }
    {
        Sector s = make_sector(layout16());
        s[100] ^= 0x01;  // entries の中身を 1 ビット壊す
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_CRC);
    }
    {
        Sector s{};
        s.fill(0xff);  // 消去済みセクタ
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_MAGIC);
    }
    {
        auto e = layout16();
        e[1].offset = 0x4FF000;  // storage が main と重なる
        Sector s = make_sector(e);
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_OVERLAP);
    }
    {
        auto e = layout16();
        e[0].offset = 0x1000;  // app が 64 KiB 境界にない
        e[0].size = 0x4FF000;
        Sector s = make_sector(e);
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_ENTRY);
    }
    {
        auto e = layout16();
        e[5].size = 0x170000;  // 予約範囲 (0xE60000) を 64 KiB はみ出す
        Sector s = make_sector(e);
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_ENTRY);
    }
    {
        auto e = layout16();
        std::strncpy(e[2].label, "storage", FL_LABEL_LEN);  // ラベル重複
        Sector s = make_sector(e);
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_OVERLAP);
    }
    {
        auto e = layout16();
        e[4].kind = 99;  // 未知だが必須ではない → 受理
        Sector s = make_sector(e);
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_OK);
        e[4].flags = FL_FLAG_REQUIRED;  // 未知かつ必須 → 拒否
        s = make_sector(e);
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_KIND);
    }
    {
        auto e = layout16();
        e[0].kind = FL_KIND_RAW;  // app が無い
        Sector s = make_sector(e);
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_NO_APP);
    }
    {
        Sector s = make_sector(layout16(), 1, kReserved + 0x1000);  // 予約範囲が 64 KiB 境界にない
        CHECK(fl_exttab_validate(s.data(), s.size(), kFlash16M, nullptr) == FL_ERR_RANGE);
    }
}

void test_exttab_select()
{
    Sector a = make_sector(layout16(), 5);
    Sector b = make_sector(layout16(), 6);
    fl_exttab_view_t va{}, vb{};
    auto sa = fl_exttab_validate(a.data(), a.size(), kFlash16M, &va);
    auto sb = fl_exttab_validate(b.data(), b.size(), kFlash16M, &vb);
    CHECK(fl_exttab_select(sa, &va, sb, &vb) == 1);
    CHECK(fl_exttab_select(sb, &vb, sa, &va) == 0);
    CHECK(fl_exttab_select(sa, &va, sa, &va) == 0);  // 同じ世代なら A
    CHECK(fl_exttab_select(FL_ERR_CRC, nullptr, sb, &vb) == 1);
    CHECK(fl_exttab_select(sa, &va, FL_ERR_MAGIC, nullptr) == 0);
    CHECK(fl_exttab_select(FL_ERR_CRC, nullptr, FL_ERR_MAGIC, nullptr) == -1);
}

fl_bootctl_t bootctl(std::uint32_t seq, fl_target target, bool pending, std::uint8_t attempts,
                     const char* tag = "")
{
    fl_bootctl_t b{};
    b.seq = seq;
    b.target = static_cast<std::uint8_t>(target);
    b.attempts = attempts;
    b.max_attempts = FL_BOOTCTL_DEFAULT_MAX_ATTEMPTS;
    b.pending = pending ? 1 : 0;
    std::strncpy(b.request_tag, tag, FL_BOOTCTL_TAG_LEN - 1);
    fl_bootctl_finalize(&b);
    return b;
}

void test_bootctl_valid_and_select()
{
    fl_bootctl_t a = bootctl(1, FL_TARGET_MAIN, false, 0);
    fl_bootctl_t b = bootctl(2, FL_TARGET_RECOVERY, false, 0, "v0.13.0");
    CHECK(fl_bootctl_valid(&a));
    CHECK(fl_bootctl_valid(&b));
    CHECK(fl_bootctl_select(&a, &b) == 1);
    CHECK(fl_bootctl_select(&b, &a) == 0);

    fl_bootctl_t erased{};
    std::memset(&erased, 0xff, sizeof(erased));
    CHECK(!fl_bootctl_valid(&erased));
    CHECK(fl_bootctl_select(&erased, &a) == 1);
    CHECK(fl_bootctl_select(&a, &erased) == 0);
    CHECK(fl_bootctl_select(&erased, &erased) == -1);

    fl_bootctl_t bad = a;
    bad.attempts ^= 1;  // CRC 不一致
    CHECK(!fl_bootctl_valid(&bad));
    fl_bootctl_t badtarget = a;
    badtarget.target = 7;
    fl_bootctl_finalize(&badtarget);
    CHECK(!fl_bootctl_valid(&badtarget));
}

void test_boot_decide()
{
    fl_bootctl_t next{};
    bool write = true;

    // 両方無効 → Recovery。
    CHECK(fl_boot_decide(nullptr, &next, &write) == FL_BOOT_RECOVERY);
    CHECK(!write);

    // 確認済み Main → 書き込み無しで Main。
    fl_bootctl_t confirmed = bootctl(10, FL_TARGET_MAIN, false, 0);
    CHECK(fl_boot_decide(&confirmed, &next, &write) == FL_BOOT_MAIN);
    CHECK(!write);

    // Recovery 指定 → Recovery。
    fl_bootctl_t rcv = bootctl(11, FL_TARGET_RECOVERY, false, 0, "v0.13.0");
    CHECK(fl_boot_decide(&rcv, &next, &write) == FL_BOOT_RECOVERY);
    CHECK(!write);

    // pending 中: 試行を数えながら Main、上限で Recovery。
    fl_bootctl_t p = bootctl(20, FL_TARGET_MAIN, true, 0);
    for (unsigned i = 0; i < FL_BOOTCTL_DEFAULT_MAX_ATTEMPTS; ++i) {
        CHECK(fl_boot_decide(&p, &next, &write) == FL_BOOT_MAIN);
        CHECK(write);
        CHECK(fl_bootctl_valid(&next));
        CHECK(next.seq == p.seq + 1);
        CHECK(next.attempts == p.attempts + 1);
        CHECK(next.pending == 1);
        p = next;
    }
    CHECK(fl_boot_decide(&p, &next, &write) == FL_BOOT_RECOVERY);
    CHECK(!write);
}

} // namespace

int main()
{
    test_crc32_known_vector();
    test_exttab_valid_layout();
    test_exttab_rejects();
    test_exttab_select();
    test_bootctl_valid_and_select();
    test_boot_decide();
    if (g_failures == 0) {
        std::printf("flash_layout format: all tests passed\n");
        return 0;
    }
    std::printf("flash_layout format: %d failure(s)\n", g_failures);
    return 1;
}
