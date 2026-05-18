// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/config_store.hpp>

#include <cstdint>
#include <cstring>
#include <utility>

#include <esp_log.h>
#include <nvs.h>

namespace stackchan::config::store {

namespace {

constexpr const char* kTag = "cfg-store";
constexpr const char* kNs = "stackchan_cfg";
constexpr const char* kKeySsid = "wifi_ssid";
constexpr const char* kKeyPass = "wifi_pass";
constexpr const char* kKeyApiKey = "openai_key";
constexpr const char* kKeyOpenAiEnabled = "openai_en";
constexpr const char* kKeyJttsConfig = "jtts_cfg";
constexpr const char* kKeyGeminiApiKey = "gemini_key";
constexpr const char* kKeyProvider = "provider";

std::string nvs_read_str(nvs_handle_t h, const char* key)
{
    std::size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) return {};
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_str(%s) size: %s", key, esp_err_to_name(err));
        return {};
    }
    std::string val(len, '\0');
    err = nvs_get_str(h, key, val.data(), &len);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_str(%s) read: %s", key, esp_err_to_name(err));
        return {};
    }
    // nvs_get_str includes the null terminator in len; remove it from the string.
    if (!val.empty() && val.back() == '\0') val.pop_back();
    return val;
}

} // namespace

DeviceConfig load()
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return {};
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open(RO): %s", esp_err_to_name(err));
        return {};
    }
    DeviceConfig cfg;
    cfg.wifi_ssid = nvs_read_str(h, kKeySsid);
    cfg.wifi_password = nvs_read_str(h, kKeyPass);
    cfg.openai_api_key = nvs_read_str(h, kKeyApiKey);
    cfg.gemini_api_key = nvs_read_str(h, kKeyGeminiApiKey);
    cfg.jtts_config_json = nvs_read_str(h, kKeyJttsConfig);
    // Default to enabled when the key is missing (pre-flag NVS contents).
    std::uint8_t enabled = 1;
    esp_err_t en_err = nvs_get_u8(h, kKeyOpenAiEnabled, &enabled);
    if (en_err == ESP_OK) {
        cfg.openai_enabled = (enabled != 0);
    } else if (en_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "nvs_get_u8(%s): %s", kKeyOpenAiEnabled, esp_err_to_name(en_err));
    }
    std::uint8_t provider = static_cast<std::uint8_t>(Provider::OpenAi);
    esp_err_t prov_err = nvs_get_u8(h, kKeyProvider, &provider);
    if (prov_err == ESP_OK) {
        cfg.provider = (provider == static_cast<std::uint8_t>(Provider::Gemini))
                           ? Provider::Gemini : Provider::OpenAi;
    } else if (prov_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "nvs_get_u8(%s): %s", kKeyProvider, esp_err_to_name(prov_err));
    }
    nvs_close(h);
    return cfg;
}

tl::expected<void, Error> save(const DeviceConfig& cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(RW): %s", esp_err_to_name(err));
        return tl::unexpected(Error::NvsInit);
    }

    const std::pair<const char*, const std::string&> entries[] = {
        {kKeySsid, cfg.wifi_ssid},
        {kKeyPass, cfg.wifi_password},
        {kKeyApiKey, cfg.openai_api_key},
        {kKeyGeminiApiKey, cfg.gemini_api_key},
        {kKeyJttsConfig, cfg.jtts_config_json},
    };
    for (const auto& [key, value] : entries) {
        err = nvs_set_str(h, key, value.c_str());
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "nvs_set_str(%s): %s", key, esp_err_to_name(err));
            nvs_close(h);
            return tl::unexpected(Error::NvsWrite);
        }
    }

    err = nvs_set_u8(h, kKeyOpenAiEnabled, cfg.openai_enabled ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_set_u8(%s): %s", kKeyOpenAiEnabled, esp_err_to_name(err));
        nvs_close(h);
        return tl::unexpected(Error::NvsWrite);
    }

    err = nvs_set_u8(h, kKeyProvider, static_cast<std::uint8_t>(cfg.provider));
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_set_u8(%s): %s", kKeyProvider, esp_err_to_name(err));
        nvs_close(h);
        return tl::unexpected(Error::NvsWrite);
    }

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_commit: %s", esp_err_to_name(err));
        return tl::unexpected(Error::NvsWrite);
    }
    return {};
}

} // namespace stackchan::config::store
