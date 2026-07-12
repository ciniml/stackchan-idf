// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/config_store.hpp>
#include <config_service/settings_registry.hpp>

#include <cstdint>
#include <cstring>

#include <esp_log.h>
#include <nvs.h>

namespace stackchan::config::store {

namespace {

constexpr const char* kTag = "cfg-store";
constexpr const char* kNs = "stackchan_cfg";

using registry::ApplyKind;
using registry::SettingDescriptor;
using registry::ValueType;

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

// Width-dispatched numeric read. Missing key leaves `cfg` at its built-in
// default; out-of-range values clamp or fall back per the descriptor.
void load_numeric(nvs_handle_t h, const SettingDescriptor& d, DeviceConfig& cfg)
{
    std::uint32_t value = 0;
    esp_err_t err = ESP_OK;
    switch (d.type) {
    case ValueType::Bool:
    case ValueType::U8: {
        std::uint8_t v = 0;
        err = nvs_get_u8(h, d.nvs_key, &v);
        value = v;
        break;
    }
    case ValueType::U16: {
        std::uint16_t v = 0;
        err = nvs_get_u16(h, d.nvs_key, &v);
        value = v;
        break;
    }
    case ValueType::U32:
        err = nvs_get_u32(h, d.nvs_key, &value);
        break;
    case ValueType::Str:
        return; // handled by the caller
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get(%s): %s", d.nvs_key, esp_err_to_name(err));
        return;
    }
    if (d.max_value != 0 && value > d.max_value) {
        if (!d.clamp) {
            ESP_LOGW(kTag, "%s: out-of-range value %lu, keeping default", d.nvs_key,
                     static_cast<unsigned long>(value));
            return;
        }
        value = d.max_value;
    }
    d.num_set(cfg, value);
}

esp_err_t save_numeric(nvs_handle_t h, const SettingDescriptor& d, const DeviceConfig& cfg)
{
    const std::uint32_t value = d.num_get(cfg);
    switch (d.type) {
    case ValueType::Bool:
    case ValueType::U8:
        return nvs_set_u8(h, d.nvs_key, static_cast<std::uint8_t>(value));
    case ValueType::U16:
        return nvs_set_u16(h, d.nvs_key, static_cast<std::uint16_t>(value));
    case ValueType::U32:
        return nvs_set_u32(h, d.nvs_key, value);
    case ValueType::Str:
        break; // handled by the caller
    }
    return ESP_OK;
}

// Persist an ad-hoc list of registry rows out of a scratch DeviceConfig.
// Backs the single-writer saves (LED tuple / mic gain / speaker volume) that
// must not run through the full save() — see the ApplyKind::Live comment in
// settings_registry.hpp.
tl::expected<void, Error> save_rows(std::initializer_list<const char*> ids,
                                    const DeviceConfig& cfg, const char* what)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(%s): %s", kNs, esp_err_to_name(err));
        return tl::unexpected(Error::NvsWrite);
    }
    for (const char* id : ids) {
        const SettingDescriptor* d = registry::find(id);
        if (!d) continue; // ids are compile-time literals; a miss is a programming error
        err = save_numeric(h, *d, cfg);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "%s: %s", what, esp_err_to_name(err));
        return tl::unexpected(Error::NvsWrite);
    }
    return {};
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
    for (const auto& d : registry::table()) {
        if (d.type == ValueType::Str) {
            cfg.*(d.str_member) = nvs_read_str(h, d.nvs_key);
        } else {
            load_numeric(h, d, cfg);
        }
    }
    // operation_mode migration: when the key is missing (older firmware),
    // synthesise a sensible mode from the legacy openai_enabled /
    // jtts_idle_enabled toggles so the device boots into the same behaviour
    // the user had before upgrading.
    {
        std::uint8_t op_mode = 0;
        if (nvs_get_u8(h, registry::find("operation-mode")->nvs_key, &op_mode) ==
            ESP_ERR_NVS_NOT_FOUND) {
            if (cfg.openai_enabled)         cfg.operation_mode = OperationMode::Conversation;
            else if (cfg.jtts_idle_enabled) cfg.operation_mode = OperationMode::JttsRandom;
            else                            cfg.operation_mode = OperationMode::MicLipSync;
        }
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

    for (const auto& d : registry::table()) {
        // ApplyKind::Live rows (LED tuple / mic gain / speaker volume) are NOT
        // written here on purpose: the full save() runs from the Apply button,
        // which merges DeviceConfig from g_active (the boot-time snapshot) +
        // staging — and live rows aren't in staging because their sinks
        // persist each change directly via the save_* single-writers below.
        // Writing them here too would clobber the user's runtime changes with
        // the stale boot value on every Apply.
        if (d.apply == ApplyKind::Live) continue;
        if (d.type == ValueType::Str) {
            err = nvs_set_str(h, d.nvs_key, (cfg.*(d.str_member)).c_str());
        } else {
            err = save_numeric(h, d, cfg);
        }
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "nvs_set(%s): %s", d.nvs_key, esp_err_to_name(err));
            nvs_close(h);
            return tl::unexpected(Error::NvsWrite);
        }
    }

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_commit: %s", esp_err_to_name(err));
        return tl::unexpected(Error::NvsWrite);
    }
    return {};
}

tl::expected<void, Error> save_one(const SettingDescriptor& d, const DeviceConfig& cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(RW) save_one: %s", esp_err_to_name(err));
        return tl::unexpected(Error::NvsInit);
    }
    if (d.type == ValueType::Str) {
        err = nvs_set_str(h, d.nvs_key, (cfg.*(d.str_member)).c_str());
    } else {
        err = save_numeric(h, d, cfg);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "save_one(%s): %s", d.nvs_key, esp_err_to_name(err));
        return tl::unexpected(Error::NvsWrite);
    }
    return {};
}

tl::expected<void, Error> save_led_state(std::uint8_t mode, std::uint32_t color,
                                         std::uint8_t brightness,
                                         std::uint8_t gradient_period_ds)
{
    DeviceConfig scratch;
    scratch.led_mode = mode;
    scratch.led_color = color;
    scratch.led_brightness = brightness;
    scratch.led_gradient_period_ds = gradient_period_ds;
    return save_rows({"led-mode", "led-color", "led-brightness", "led-gradient-period"},
                     scratch, "save_led_state");
}

tl::expected<void, Error> save_mic_lip_gain(std::uint16_t input_pct,
                                            std::uint16_t output_pct)
{
    DeviceConfig scratch;
    scratch.mic_lip_input_gain_pct = input_pct;
    scratch.mic_lip_output_gain_pct = output_pct;
    return save_rows({"mic-lip-input-gain", "mic-lip-output-gain"}, scratch,
                     "save_mic_lip_gain");
}

tl::expected<void, Error> save_speaker_volume(std::uint16_t pct)
{
    if (pct > 200) pct = 200;
    DeviceConfig scratch;
    scratch.speaker_volume_pct = pct;
    return save_rows({"speaker-volume"}, scratch, "save_speaker_volume");
}

} // namespace stackchan::config::store
