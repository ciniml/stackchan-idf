// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "hmm_voice.hpp"

#include "sdkconfig.h"

#include <cstring>

#include <esp_crc.h>
#include <esp_log.h>
#include <esp_partition.h>
#if defined(CONFIG_JTTS_ENABLE_HMM)
#include <esp_ota_ops.h>
#endif

#include <jtts/jtts.hpp>

namespace stackchan::app::hmm_voice {

namespace {

constexpr const char* kTag = "hmm-voice";
constexpr const char* kPart = "voice";
constexpr char kMagic[4] = {'H', 'V', 'O', 'X'};

struct Header {
    char magic[4];
    std::uint32_t size;   // データ部のバイト数
    std::uint32_t crc32;  // データ部の CRC32 (esp_crc32_le, seed 0xFFFFFFFF)
    std::uint32_t reserved;
};
static_assert(sizeof(Header) == 16);

const esp_partition_t* g_part = nullptr;
bool g_using_ota_slot = false;  // 待機 OTA スロットを流用しているか
esp_partition_mmap_handle_t g_mmap = 0;
const void* g_mapped = nullptr;
std::uint32_t g_mapped_size = 0;

// HMM ボイスの格納先パーティションを決める:
//   1. 専用 "voice" パーティション (16 MB ボード = partitions_16mb.csv) を優先。
//   2. 無ければ (8 MB ボード = partitions_8mb.csv, AtomS3R 等) HMM が有効な
//      ビルドに限り、**待機側 OTA スロット** (esp_ota_get_next_update_partition)
//      を流用する。実行中アプリとは別スロットなので通常起動には影響しない。
//      トレードオフ: OTA 更新するとこのスロットにアプリが書かれてボイスは消える
//      (要再取得)。ロールバック先は「更新前に動いていた側」なので、OTA が
//      待機側 (=ボイス側) を上書きしてから起動する構造上、ロールバックの安全網
//      自体は保たれる (消えるのはボイスのみ)。
//   HMM 無効ビルドでは OTA スロットには触れない (ボイスを置く意味が無く、
//   誤って待機アプリ イメージを破壊しないため)。
const esp_partition_t* find_partition() {
    if (g_part == nullptr) {
        g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPart);
#if defined(CONFIG_JTTS_ENABLE_HMM)
        if (g_part == nullptr) {
            g_part = esp_ota_get_next_update_partition(nullptr);
            g_using_ota_slot = (g_part != nullptr);
        }
#endif
    }
    return g_part;
}

void unmap() {
    if (g_mmap != 0) {
        esp_partition_munmap(g_mmap);
        g_mmap = 0;
        g_mapped = nullptr;
        g_mapped_size = 0;
    }
}

// ヘッダを読んで妥当ならデータ部サイズを返す (0 = 無し/壊れ)。
std::uint32_t read_header(const esp_partition_t* part) {
    Header h;
    if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK) return 0;
    if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) return 0;
    if (h.size == 0 || h.size > part->size - sizeof(Header)) return 0;
    return h.size;
}

// パーティションのデータ部を mmap して jtts に登録する。
bool map_and_load(const esp_partition_t* part, std::uint32_t size, bool check_crc) {
    unmap();
    const void* p = nullptr;
    esp_partition_mmap_handle_t mh = 0;
    if (esp_partition_mmap(part, 0, sizeof(Header) + size, ESP_PARTITION_MMAP_DATA, &p, &mh) !=
        ESP_OK) {
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
    if (!jtts::set_hmm_voice({data, size})) {
        ESP_LOGW(kTag, ".htsvoice parse failed (%u B)", static_cast<unsigned>(size));
        esp_partition_munmap(mh);
        return false;
    }
    g_mmap = mh;
    g_mapped = p;
    g_mapped_size = size;
    return true;
}

}  // namespace

bool init() {
    const esp_partition_t* part = find_partition();
    if (part == nullptr) {
        ESP_LOGI(kTag, "no HMM voice storage (no \"%s\" partition / HMM disabled)", kPart);
        return false;
    }
    if (g_using_ota_slot) {
        ESP_LOGI(kTag, "HMM voice storage: spare OTA slot \"%s\" (%u KiB)", part->label,
                 static_cast<unsigned>(part->size / 1024));
    }
    const std::uint32_t size = read_header(part);
    // OTA スロット流用時、更新直後はここに旧アプリ イメージが入っている
    // (HVOX マジック無し → size==0)。その場合はボイス未登録として扱う。
    if (size == 0) return false;
    if (!map_and_load(part, size, /*check_crc=*/true)) return false;
    ESP_LOGI(kTag, "HMM voice loaded (%u B)", static_cast<unsigned>(size));
    return true;
}

const char* store(std::span<const std::uint8_t> data) {
    if (data.empty()) return "empty body";
    const esp_partition_t* part = find_partition();
    if (part == nullptr) return "HMM voice storage unavailable on this board";
    if (data.size() > part->size - sizeof(Header)) return "voice too large for partition";
    // ざっくり検証: .htsvoice は "[GLOBAL]" で始まるテキスト ヘッダを持つ
    if (data.size() < 16 || std::memcmp(data.data(), "[GLOBAL]", 8) != 0) {
        return "not a .htsvoice image";
    }

    // 書き込み中は旧イメージを参照させない (mmap を先に解除)
    jtts::set_hmm_voice({});
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
        return "htsvoice load failed (bad image or out of memory)";
    }
    ESP_LOGI(kTag, "HMM voice stored + loaded (%u B)", static_cast<unsigned>(data.size()));
    return nullptr;
}

const char* clear() {
    jtts::set_hmm_voice({});
    unmap();
    const esp_partition_t* part = find_partition();
    if (part != nullptr) {
        // ヘッダ セクタだけ消せば無効になる
        esp_partition_erase_range(part, 0, part->erase_size);
    }
    ESP_LOGI(kTag, "HMM voice cleared");
    return nullptr;
}

Status status() {
    Status st;
    st.loaded = jtts::hmm_voice_loaded();
    const esp_partition_t* part = find_partition();
    if (part != nullptr) {
        st.capacity = part->size - sizeof(Header);
        st.stored_bytes = read_header(part);
    }
    return st;
}

}  // namespace stackchan::app::hmm_voice
