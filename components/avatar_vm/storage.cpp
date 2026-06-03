// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar_vm/storage.hpp"

#include <esp_log.h>
#include <nvs.h>

namespace stackchan::avatar_vm::storage {

namespace {
constexpr const char* kTag = "avatar_vm";
constexpr const char* kNs = "avatar_vm";
constexpr const char* kKey = "face_bc";
} // namespace

const char* to_string(StorageError e) noexcept
{
    switch (e) {
    case StorageError::NvsOpen: return "nvs_open failed";
    case StorageError::NotFound: return "not found";
    case StorageError::ReadFailed: return "nvs read failed";
    case StorageError::WriteFailed: return "nvs write failed";
    case StorageError::CommitFailed: return "nvs commit failed";
    case StorageError::EraseFailed: return "nvs erase failed";
    case StorageError::TooLarge: return "bytecode too large";
    }
    return "?";
}

tl::expected<std::vector<std::uint8_t>, StorageError> load() noexcept
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return tl::unexpected(StorageError::NotFound);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open(RO): %s", esp_err_to_name(err));
        return tl::unexpected(StorageError::NvsOpen);
    }
    std::size_t len = 0;
    err = nvs_get_blob(h, kKey, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) {
        nvs_close(h);
        return tl::unexpected(StorageError::NotFound);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_blob(size): %s", esp_err_to_name(err));
        nvs_close(h);
        return tl::unexpected(StorageError::ReadFailed);
    }
    std::vector<std::uint8_t> buf(len);
    err = nvs_get_blob(h, kKey, buf.data(), &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_blob(read): %s", esp_err_to_name(err));
        return tl::unexpected(StorageError::ReadFailed);
    }
    return buf;
}

tl::expected<void, StorageError> save(std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() > kMaxBytecodeBytes) {
        return tl::unexpected(StorageError::TooLarge);
    }
    // Validate before persisting — never store a blob the VM cannot run.
    auto bc = decode(bytes);
    if (!bc) {
        ESP_LOGW(kTag, "save: decode rejected (%s)", to_string(bc.error()));
        return tl::unexpected(StorageError::WriteFailed);
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open(RW): %s", esp_err_to_name(err));
        return tl::unexpected(StorageError::NvsOpen);
    }
    err = nvs_set_blob(h, kKey, bytes.data(), bytes.size());
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_set_blob: %s", esp_err_to_name(err));
        nvs_close(h);
        return tl::unexpected(StorageError::WriteFailed);
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_commit: %s", esp_err_to_name(err));
        return tl::unexpected(StorageError::CommitFailed);
    }
    return {};
}

tl::expected<void, StorageError> clear() noexcept
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return tl::unexpected(StorageError::NvsOpen);
    }
    err = nvs_erase_key(h, kKey);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return {}; // already absent
    }
    if (err != ESP_OK) {
        nvs_close(h);
        return tl::unexpected(StorageError::EraseFailed);
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        return tl::unexpected(StorageError::CommitFailed);
    }
    return {};
}

} // namespace stackchan::avatar_vm::storage
