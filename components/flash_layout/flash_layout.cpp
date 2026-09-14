// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "flash_layout/flash_layout.hpp"

#include <array>
#include <cstring>
#include <optional>

#include <esp_flash.h>
#include <esp_log.h>

namespace stackchan::flash_layout {

namespace {

constexpr const char* kTag = "flash-layout";

// 採用したテーブルのコピー (view はこのバッファを指す)。
std::array<std::uint8_t, FL_SECTOR_SIZE> g_sector;
fl_exttab_view_t g_view{};
std::optional<tl::expected<Info, Error>> g_init_result;
// 登録した子領域。エントリ順。未登録 (未知 kind) は nullptr。
std::array<const esp_partition_t*, FL_EXTTAB_MAX_ENTRIES> g_children{};

const esp_partition_t* find_std(esp_partition_subtype_t subtype)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, nullptr);
}

std::uint32_t flash_size()
{
    std::uint32_t size = 0;
    if (esp_flash_get_size(nullptr, &size) != ESP_OK) return 0;
    return size;
}

bool register_children(const fl_exttab_view_t& view, std::uint32_t reserved_offset)
{
    for (std::uint16_t i = 0; i < view.entry_count; ++i) {
        const fl_exttab_entry_t& e = view.entries[i];
        esp_partition_type_t type = ESP_PARTITION_TYPE_DATA;
        esp_partition_subtype_t subtype = kRawSubtype;
        switch (e.kind) {
        case FL_KIND_APP:
            type = ESP_PARTITION_TYPE_APP;
            subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
            break;
        case FL_KIND_SPIFFS:   subtype = ESP_PARTITION_SUBTYPE_DATA_SPIFFS; break;
        case FL_KIND_LITTLEFS: subtype = ESP_PARTITION_SUBTYPE_DATA_LITTLEFS; break;
        case FL_KIND_RAW:      subtype = kRawSubtype; break;
        default:
            ESP_LOGW(kTag, "entry '%s': unknown kind %u — not registered", e.label, e.kind);
            g_children[i] = nullptr;
            continue;
        }
        const esp_partition_t* p = nullptr;
        esp_err_t err = esp_partition_register_external(nullptr, reserved_offset + e.offset, e.size, e.label,
                                                        type, subtype, &p);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "register '%s' @0x%08lx +0x%lx: %s", e.label,
                     static_cast<unsigned long>(reserved_offset + e.offset), static_cast<unsigned long>(e.size),
                     esp_err_to_name(err));
            return false;
        }
        g_children[i] = p;
        ESP_LOGI(kTag, "  %-12s 0x%08lx 0x%08lx kind=%u", e.label,
                 static_cast<unsigned long>(reserved_offset + e.offset), static_cast<unsigned long>(e.size),
                 e.kind);
    }
    return true;
}

// --- bootctl helpers ---

struct BootctlSlots {
    const esp_partition_t* part;
    fl_bootctl_t a;
    fl_bootctl_t b;
    int active;  // 0 / 1 / -1
};

tl::expected<BootctlSlots, Error> read_slots()
{
    const esp_partition_t* part = find_std(kBootctlSubtype);
    if (part == nullptr || part->size < 2 * FL_SECTOR_SIZE) return tl::unexpected(Error::NoBootctlPartition);
    BootctlSlots s{};
    s.part = part;
    if (esp_partition_read(part, 0, &s.a, sizeof(s.a)) != ESP_OK) return tl::unexpected(Error::FlashRead);
    if (esp_partition_read(part, FL_SECTOR_SIZE, &s.b, sizeof(s.b)) != ESP_OK) return tl::unexpected(Error::FlashRead);
    s.active = fl_bootctl_select(&s.a, &s.b);
    return s;
}

// 現在の有効コピーを base にして mutate を適用し、もう片方のセクタへ seq+1 で書く。
template <typename F>
tl::expected<void, Error> update_bootctl(F&& mutate)
{
    auto slots = read_slots();
    if (!slots) return tl::unexpected(slots.error());

    fl_bootctl_t next{};
    if (slots->active >= 0) {
        next = (slots->active == 0) ? slots->a : slots->b;
        next.seq += 1;
    } else {
        next.seq = 1;
        next.max_attempts = FL_BOOTCTL_DEFAULT_MAX_ATTEMPTS;
    }
    if (auto r = mutate(next); !r) return r;
    fl_bootctl_finalize(&next);

    // 有効側が A なら B へ、B なら A へ。両方無効なら A へ。
    const std::uint32_t off = (slots->active == 0) ? FL_SECTOR_SIZE : 0;
    if (esp_partition_erase_range(slots->part, off, FL_SECTOR_SIZE) != ESP_OK) return tl::unexpected(Error::FlashWrite);
    if (esp_partition_write(slots->part, off, &next, sizeof(next)) != ESP_OK) return tl::unexpected(Error::FlashWrite);

    // 読み戻して確認 (ADR-001「更新開始と電断復旧」)。
    fl_bootctl_t check{};
    if (esp_partition_read(slots->part, off, &check, sizeof(check)) != ESP_OK) return tl::unexpected(Error::FlashRead);
    if (std::memcmp(&check, &next, sizeof(next)) != 0 || !fl_bootctl_valid(&check)) {
        ESP_LOGE(kTag, "bootctl verify failed at slot %c", off == 0 ? 'A' : 'B');
        return tl::unexpected(Error::FlashWrite);
    }
    ESP_LOGI(kTag, "bootctl seq=%lu target=%s pending=%u attempts=%u tag='%s' -> slot %c",
             static_cast<unsigned long>(next.seq), next.target == FL_TARGET_MAIN ? "main" : "recovery",
             next.pending, next.attempts, next.request_tag, off == 0 ? 'A' : 'B');
    return {};
}

} // namespace

tl::expected<Info, Error> init()
{
    if (g_init_result.has_value()) return *g_init_result;

    auto fail = [](Error e) {
        g_init_result = tl::unexpected(e);
        return *g_init_result;
    };

    const esp_partition_t* part = find_std(kExttabSubtype);
    if (part == nullptr || part->size < 2 * FL_SECTOR_SIZE) return fail(Error::NoExttabPartition);

    const std::uint32_t fsize = flash_size();
    std::array<std::uint8_t, FL_SECTOR_SIZE> other{};
    fl_exttab_view_t va{}, vb{};
    if (esp_partition_read(part, 0, g_sector.data(), g_sector.size()) != ESP_OK) return fail(Error::FlashRead);
    if (esp_partition_read(part, FL_SECTOR_SIZE, other.data(), other.size()) != ESP_OK) return fail(Error::FlashRead);
    const enum fl_status sa = fl_exttab_validate(g_sector.data(), g_sector.size(), fsize, &va);
    const enum fl_status sb = fl_exttab_validate(other.data(), other.size(), fsize, &vb);
    const int sel = fl_exttab_select(sa, &va, sb, &vb);
    if (sel < 0) {
        ESP_LOGE(kTag, "exttab invalid: A=%d B=%d", static_cast<int>(sa), static_cast<int>(sb));
        return fail(Error::ExttabInvalid);
    }
    if (sel == 1) {
        g_sector = other;
        // view はコピー先を指し直す。
        (void)fl_exttab_validate(g_sector.data(), g_sector.size(), fsize, &g_view);
    } else {
        g_view = va;
    }

    Info info{};
    info.generation = g_view.header->generation;
    info.active_copy = sel;
    info.reserved_offset = g_view.header->reserved_offset;
    info.reserved_size = g_view.header->reserved_size;
    info.entry_count = g_view.entry_count;
    ESP_LOGI(kTag, "exttab gen=%lu copy=%c reserved=0x%08lx +0x%lx entries=%u",
             static_cast<unsigned long>(info.generation), sel == 0 ? 'A' : 'B',
             static_cast<unsigned long>(info.reserved_offset), static_cast<unsigned long>(info.reserved_size),
             info.entry_count);
    if (!register_children(g_view, info.reserved_offset)) return fail(Error::RegisterFailed);

    g_init_result = info;
    return *g_init_result;
}

const esp_partition_t* find(std::string_view label)
{
    if (!g_init_result.has_value() || !g_init_result->has_value()) return nullptr;
    for (std::uint16_t i = 0; i < g_view.entry_count; ++i) {
        if (label == g_view.entries[i].label) return g_children[i];
    }
    return nullptr;
}

tl::expected<fl_bootctl_t, Error> read_bootctl()
{
    auto slots = read_slots();
    if (!slots) return tl::unexpected(slots.error());
    if (slots->active < 0) return tl::unexpected(Error::BootctlInvalid);
    return slots->active == 0 ? slots->a : slots->b;
}

tl::expected<void, Error> confirm_boot()
{
    return update_bootctl([](fl_bootctl_t& b) -> tl::expected<void, Error> {
        b.target = FL_TARGET_MAIN;
        b.pending = 0;
        b.attempts = 0;
        return {};
    });
}

tl::expected<void, Error> request_recovery(std::string_view tag)
{
    if (tag.size() >= FL_BOOTCTL_TAG_LEN) return tl::unexpected(Error::TagTooLong);
    return update_bootctl([tag](fl_bootctl_t& b) -> tl::expected<void, Error> {
        b.target = FL_TARGET_RECOVERY;
        std::memset(b.request_tag, 0, sizeof(b.request_tag));
        std::memcpy(b.request_tag, tag.data(), tag.size());
        return {};
    });
}

tl::expected<void, Error> arm_main()
{
    return update_bootctl([](fl_bootctl_t& b) -> tl::expected<void, Error> {
        b.target = FL_TARGET_MAIN;
        b.pending = 1;
        b.attempts = 0;
        if (b.max_attempts == 0) b.max_attempts = FL_BOOTCTL_DEFAULT_MAX_ATTEMPTS;
        std::memset(b.request_tag, 0, sizeof(b.request_tag));
        return {};
    });
}

tl::expected<void, Error> return_to_main()
{
    return update_bootctl([](fl_bootctl_t& b) -> tl::expected<void, Error> {
        b.target = FL_TARGET_MAIN;
        b.pending = 0;
        b.attempts = 0;
        std::memset(b.request_tag, 0, sizeof(b.request_tag));
        return {};
    });
}

tl::expected<void, Error> clear_request_tag()
{
    return update_bootctl([](fl_bootctl_t& b) -> tl::expected<void, Error> {
        std::memset(b.request_tag, 0, sizeof(b.request_tag));
        return {};
    });
}

const char* error_name(Error e)
{
    switch (e) {
    case Error::NoExttabPartition:  return "no exttab partition";
    case Error::NoBootctlPartition: return "no bootctl partition";
    case Error::ExttabInvalid:      return "exttab invalid";
    case Error::BootctlInvalid:     return "bootctl invalid";
    case Error::FlashRead:          return "flash read";
    case Error::FlashWrite:         return "flash write";
    case Error::RegisterFailed:     return "register failed";
    case Error::TagTooLong:         return "tag too long";
    case Error::NotInitialized:     return "not initialized";
    }
    return "?";
}

} // namespace stackchan::flash_layout
