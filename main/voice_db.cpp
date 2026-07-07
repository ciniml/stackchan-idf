// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "voice_db.hpp"

#include <cstring>
#include <memory>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <jtts/jtts.hpp>
#include <jtts/jvox.hpp>

namespace stackchan::app::voice_db {

namespace {

constexpr const char* kTag = "voice-db";
constexpr const char* kPart = "storage";  // avatar_vm と同じ専用 NVS パーティション
constexpr const char* kNs = "jvox";
constexpr const char* kKey = "db";
// NVS blob の実装上限 (508,000 B) にマージンを持たせた受け入れ上限。
constexpr std::size_t kMaxStoredBytes = 480 * 1024;

struct FreeDeleter {
    void operator()(void* p) const { heap_caps_free(p); }
};
using PsramBuf = std::unique_ptr<std::uint8_t[], FreeDeleter>;

PsramBuf psram_alloc(std::size_t n) {
    return PsramBuf(static_cast<std::uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM)));
}

// jtts に渡している展開済み blob。差し替え時は旧世代を 1 つ残す
// (合成中タスクが旧 blob の PCM を読んでいる可能性があるため —
// Db ビュー自体は jtts 内で shared_ptr 差し替え)。
PsramBuf g_active;
std::size_t g_active_size = 0;
PsramBuf g_retired;

// "storage" パーティションの NVS を使えるようにする。avatar_vm 側が先に
// 初期化していても、こちらが先でもよいように、init 失敗時は open を試す。
bool ensure_partition() {
    static bool inited = false;
    if (inited) return true;
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPart);
    if (part == nullptr) {
        ESP_LOGW(kTag, "no \"%s\" partition", kPart);
        return false;
    }
    esp_err_t err = nvs_flash_init_partition_ptr(part);
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
        err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 既に初期化済み等 — open できるかで最終判断する。
        ESP_LOGD(kTag, "nvs init on \"%s\": %s (open で確認)", kPart, esp_err_to_name(err));
    }
    inited = true;
    return true;
}

// 生 (codec=0) blob を PSRAM に用意する。ADPCM ならここで展開。
// 成功で raw のサイズ、失敗 0。
std::size_t prepare_raw(std::span<const std::uint8_t> data, PsramBuf& out) {
    if (jtts::jvox::is_adpcm(data)) {
        const std::size_t n = jtts::jvox::decoded_size(data);
        if (n == 0) return 0;
        out = psram_alloc(n);
        if (!out) {
            ESP_LOGW(kTag, "PSRAM %u B 確保失敗 (no-PSRAM ボード?)", static_cast<unsigned>(n));
            return 0;
        }
        if (!jtts::jvox::decode_adpcm(data, {out.get(), n})) return 0;
        return n;
    }
    // 生形式: そのままコピー (blob の寿命をこちらで持つ)。
    out = psram_alloc(data.size());
    if (!out) return 0;
    std::memcpy(out.get(), data.data(), data.size());
    return data.size();
}

// 展開済み blob を jtts に登録して active に据える。
const char* activate(PsramBuf&& raw, std::size_t size) {
    if (!jtts::set_voice_db({raw.get(), size})) {
        return "jvox parse failed";
    }
    g_retired = std::move(g_active);  // 旧世代を 1 つだけ保持
    g_active = std::move(raw);
    g_active_size = size;
    return nullptr;
}

}  // namespace

std::uint16_t init() {
    if (!ensure_partition()) return 0;
    nvs_handle_t h;
    if (nvs_open_from_partition(kPart, kNs, NVS_READONLY, &h) != ESP_OK) return 0;
    std::size_t stored = 0;
    esp_err_t err = nvs_get_blob(h, kKey, nullptr, &stored);
    if (err != ESP_OK || stored == 0) {
        nvs_close(h);
        return 0;
    }
    PsramBuf packed = psram_alloc(stored);
    if (!packed) {
        nvs_close(h);
        ESP_LOGW(kTag, "PSRAM 不足で音声 DB をロードできません (%u B)",
                 static_cast<unsigned>(stored));
        return 0;
    }
    err = nvs_get_blob(h, kKey, packed.get(), &stored);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_blob: %s", esp_err_to_name(err));
        return 0;
    }
    PsramBuf raw;
    const std::size_t n = prepare_raw({packed.get(), stored}, raw);
    if (n == 0 || activate(std::move(raw), n) != nullptr) {
        ESP_LOGW(kTag, "保存済み音声 DB の展開/パースに失敗 (%u B)",
                 static_cast<unsigned>(stored));
        return 0;
    }
    const std::uint16_t units = jtts::voice_db_units();
    ESP_LOGI(kTag, "voice DB loaded: %u units (stored %u B, raw %u B)", units,
             static_cast<unsigned>(stored), static_cast<unsigned>(n));
    return units;
}

const char* store(std::span<const std::uint8_t> data) {
    if (data.empty()) return "empty body";
    if (data.size() > kMaxStoredBytes) return "voice DB too large (max 480 KiB stored)";
    if (!ensure_partition()) return "no storage partition";

    // 先に検証 (展開 + パース) — 壊れた blob を NVS に書かない。
    PsramBuf raw;
    const std::size_t n = prepare_raw(data, raw);
    if (n == 0) return "jvox decode failed (bad blob or no PSRAM)";
    if (!jtts::jvox::Db::parse({raw.get(), n})) return "jvox parse failed";

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(kPart, kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return "nvs open failed";
    err = nvs_set_blob(h, kKey, data.data(), data.size());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs write: %s", esp_err_to_name(err));
        return "nvs write failed";
    }
    const char* e = activate(std::move(raw), n);
    if (e) return e;
    ESP_LOGI(kTag, "voice DB stored + loaded: %u units (%u B)", jtts::voice_db_units(),
             static_cast<unsigned>(data.size()));
    return nullptr;
}

const char* clear() {
    if (ensure_partition()) {
        nvs_handle_t h;
        if (nvs_open_from_partition(kPart, kNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, kKey);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    jtts::set_voice_db({});
    g_retired = std::move(g_active);
    g_active_size = 0;
    ESP_LOGI(kTag, "voice DB cleared");
    return nullptr;
}

Status status() {
    Status st;
    st.units = jtts::voice_db_units();
    st.loaded = st.units > 0;
    if (ensure_partition()) {
        nvs_handle_t h;
        if (nvs_open_from_partition(kPart, kNs, NVS_READONLY, &h) == ESP_OK) {
            std::size_t stored = 0;
            if (nvs_get_blob(h, kKey, nullptr, &stored) == ESP_OK) {
                st.stored_bytes = static_cast<std::uint32_t>(stored);
            }
            nvs_close(h);
        }
    }
    return st;
}

}  // namespace stackchan::app::voice_db
