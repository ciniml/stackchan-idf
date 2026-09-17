// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "sano_weights.hpp"

#include <sdkconfig.h>

#include <cstring>

#include <esp_crc.h>
#include <esp_log.h>
#include <esp_partition.h>

#include <jtts/jtts.hpp>

namespace stackchan::app::sano_weights {

namespace {

constexpr const char* kTag = "sano-weights";
constexpr const char* kPart = "sanotts";
constexpr char kMagic[4] = {'S', 'A', 'N', 'W'};
// blob 自体の先頭 (上流 saan_weights_open が検証する "SAAN")。
constexpr char kBlobMagic[4] = {'S', 'A', 'A', 'N'};

struct Header {
    char magic[4];
    std::uint32_t size;   // データ部のバイト数
    std::uint32_t crc32;  // データ部の CRC32 (esp_crc32_le, seed 0xFFFFFFFF)
    std::uint32_t reserved;
};
static_assert(sizeof(Header) == 16);  // データ部を 16 バイト境界に置く (PIE の要件)

const esp_partition_t* g_part = nullptr;
esp_partition_mmap_handle_t g_mmap = 0;

const esp_partition_t* find_partition() {
    if (g_part == nullptr) {
        g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPart);
    }
    return g_part;
}

void unmap() {
    if (g_mmap != 0) {
        esp_partition_munmap(g_mmap);
        g_mmap = 0;
    }
}

std::uint32_t read_header(const esp_partition_t* part) {
    Header h;
    if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK) return 0;
    if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) return 0;
    if (h.size == 0 || h.size > part->size - sizeof(Header)) return 0;
    return h.size;
}

bool map_and_load(const esp_partition_t* part, std::uint32_t size, bool check_crc) {
    jtts::set_sano_weights({});
    unmap();
    const void* p = nullptr;
    esp_partition_mmap_handle_t mh = 0;
    if (esp_partition_mmap(part, 0, sizeof(Header) + size, ESP_PARTITION_MMAP_DATA, &p, &mh) != ESP_OK) {
        ESP_LOGW(kTag, "mmap failed (%u B)", static_cast<unsigned>(size));
        return false;
    }
    const auto* data = static_cast<const std::uint8_t*>(p) + sizeof(Header);
    if (check_crc) {
        Header h;
        std::memcpy(&h, p, sizeof(h));
        const std::uint32_t crc = esp_crc32_le(0xFFFFFFFFu, data, size);
        if (crc != h.crc32) {
            ESP_LOGW(kTag, "CRC mismatch (stored %08x, calc %08x)", static_cast<unsigned>(h.crc32),
                     static_cast<unsigned>(crc));
            esp_partition_munmap(mh);
            return false;
        }
    }
    if (!jtts::set_sano_weights({data, size})) {
        ESP_LOGW(kTag, "weights rejected (%u B)", static_cast<unsigned>(size));
        esp_partition_munmap(mh);
        return false;
    }
    g_mmap = mh;
    return true;
}

}  // namespace

bool init() {
    const esp_partition_t* part = find_partition();
    if (part == nullptr) {
        ESP_LOGI(kTag, "no \"%s\" partition — sanoTTS disabled", kPart);
        return false;
    }
    const std::uint32_t size = read_header(part);
    if (size == 0) {
        ESP_LOGI(kTag, "no weights stored (capacity %u KiB)", static_cast<unsigned>(part->size / 1024));
        return false;
    }
    if (!map_and_load(part, size, /*check_crc=*/true)) return false;
    ESP_LOGI(kTag, "sanoTTS weights loaded (%u B)", static_cast<unsigned>(size));
    return true;
}

const char* store(std::span<const std::uint8_t> data) {
#if !CONFIG_JTTS_ENABLE_SANOTTS
    // エンジンがスタブのビルドでは、保存してもロードは必ず失敗する。領域を消す前に断る。
    (void)data;
    return "sanoTTS engine is not built into this firmware";
#endif
    if (data.empty()) return "empty body";
    const esp_partition_t* part = find_partition();
    if (part == nullptr) return "sanotts storage unavailable on this board";
    if (data.size() > part->size - sizeof(Header)) return "weights too large for partition";
    if (data.size() < 16 || std::memcmp(data.data(), kBlobMagic, sizeof(kBlobMagic)) != 0) {
        return "not a sanoTTS weights blob (SAAN header missing)";
    }

    jtts::set_sano_weights({});
    unmap();

    const std::size_t erase_len =
        (sizeof(Header) + data.size() + part->erase_size - 1) / part->erase_size * part->erase_size;
    esp_err_t err = esp_partition_erase_range(part, 0, erase_len);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "erase: %s", esp_err_to_name(err));
        return "flash erase failed";
    }
    Header h;
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.size = static_cast<std::uint32_t>(data.size());
    h.crc32 = esp_crc32_le(0xFFFFFFFFu, data.data(), data.size());
    h.reserved = 0;
    err = esp_partition_write(part, 0, &h, sizeof(h));
    if (err == ESP_OK) err = esp_partition_write(part, sizeof(h), data.data(), data.size());
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "write: %s", esp_err_to_name(err));
        return "flash write failed";
    }
    if (!map_and_load(part, h.size, /*check_crc=*/false)) {
        return "weights load failed (bad blob)";
    }
    ESP_LOGI(kTag, "sanoTTS weights stored + loaded (%u B)", static_cast<unsigned>(data.size()));
    return nullptr;
}

const char* clear() {
    jtts::set_sano_weights({});
    unmap();
    const esp_partition_t* part = find_partition();
    if (part != nullptr) {
        esp_partition_erase_range(part, 0, part->erase_size);
    }
    ESP_LOGI(kTag, "sanoTTS weights cleared");
    return nullptr;
}

Status status() {
    Status st;
#if CONFIG_JTTS_ENABLE_SANOTTS
    st.supported = true;
#endif
    st.loaded = jtts::sano_weights_loaded();
    const esp_partition_t* part = find_partition();
    if (part != nullptr) {
        st.capacity = part->size - sizeof(Header);
        st.stored_bytes = read_header(part);
    }
    return st;
}

}  // namespace stackchan::app::sano_weights
