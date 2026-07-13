// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/settings_registry.hpp>

#include <cstring>

namespace stackchan::config::registry {

namespace {

// Shorthand for the accessor pairs so the table below stays one row per line
// of intent. Captureless lambdas decay to the plain function pointers the
// descriptor stores.
constexpr SettingDescriptor str_row(const char* id, const char* nvs_key, ApplyKind apply,
                                    std::string DeviceConfig::* member,
                                    std::uint16_t max_len, bool secret = false)
{
    return SettingDescriptor{id, nvs_key, ValueType::Str, apply, secret,
                             false, 0, max_len, member, nullptr, nullptr};
}

constexpr SettingDescriptor num_row(const char* id, const char* nvs_key, ValueType type,
                                    ApplyKind apply, std::uint32_t max_value, bool clamp,
                                    std::uint32_t (*get)(const DeviceConfig&),
                                    void (*set)(DeviceConfig&, std::uint32_t))
{
    return SettingDescriptor{id, nvs_key, type, apply, false,
                             clamp, max_value, 0, nullptr, get, set};
}

constexpr SettingDescriptor bool_row(const char* id, const char* nvs_key, ApplyKind apply,
                                     std::uint32_t (*get)(const DeviceConfig&),
                                     void (*set)(DeviceConfig&, std::uint32_t))
{
    return num_row(id, nvs_key, ValueType::Bool, apply, 1, true, get, set);
}

// The single source of truth for "what settings exist". The nvs_key column
// reproduces the constants that used to live at the top of config_store.cpp
// byte-for-byte; the id column matches the per-key HTTP route suffix where
// one exists (POST /api/<id>). Both columns are wire contracts.
const std::array<SettingDescriptor, kSettingCount> kTable = {{
    // --- strings ---------------------------------------------------------
    // max_len mirrors the HTTP kMax* / BLE plaintext caps for each field.
    str_row("ssid",           "wifi_ssid",  ApplyKind::Staged, &DeviceConfig::wifi_ssid, 32),
    str_row("password",       "wifi_pass",  ApplyKind::Staged, &DeviceConfig::wifi_password, 64, true),
    str_row("api-key",        "openai_key", ApplyKind::Staged, &DeviceConfig::openai_api_key, 256, true),
    str_row("gemini-api-key", "gemini_key", ApplyKind::Staged, &DeviceConfig::gemini_api_key, 256, true),
    str_row("xiaozhi-url",    "xz_url",     ApplyKind::Staged, &DeviceConfig::xiaozhi_url, 256),
    str_row("xiaozhi-token",  "xz_token",   ApplyKind::Staged, &DeviceConfig::xiaozhi_token, 256, true),
    str_row("jtts-config",    "jtts_cfg",   ApplyKind::Staged, &DeviceConfig::jtts_config_json, 960),
    str_row("system-prompt",  "sys_prompt", ApplyKind::Staged, &DeviceConfig::system_prompt, 2048),
    str_row("conv-headers",   "conv_hdrs",  ApplyKind::Staged, &DeviceConfig::conv_extra_headers, 1024, true),
    str_row("face-config",    "face_cfg",   ApplyKind::Both,   &DeviceConfig::face_config_json, 768),
    str_row("servo-limits",   "srv_lim",    ApplyKind::Staged, &DeviceConfig::servo_limits_json, 768),
    str_row("mcp-token",      "mcp_token",  ApplyKind::Both,   &DeviceConfig::mcp_api_token, 128, true),
    str_row("lt-config",      "lt_cfg",     ApplyKind::Both,   &DeviceConfig::lt_config_json, 768),
    str_row("device-name",    "dev_name",   ApplyKind::Staged, &DeviceConfig::device_name, 24),
    str_row("auth-password",  "auth_pwd",   ApplyKind::Both,   &DeviceConfig::auth_password, 64, true),
    // --- bools ------------------------------------------------------------
    bool_row("openai-enabled",    "openai_en",  ApplyKind::Staged,
             [](const DeviceConfig& c) -> std::uint32_t { return c.openai_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.openai_enabled = (v != 0); }),
    bool_row("rtp-enabled",       "rtp_en",     ApplyKind::Staged,
             [](const DeviceConfig& c) -> std::uint32_t { return c.rtp_audio_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.rtp_audio_enabled = (v != 0); }),
    bool_row("jtts-idle-enabled", "jtts_idle",  ApplyKind::Staged,
             [](const DeviceConfig& c) -> std::uint32_t { return c.jtts_idle_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.jtts_idle_enabled = (v != 0); }),
    bool_row("battery-gauge",     "bat_gauge",  ApplyKind::Staged,
             [](const DeviceConfig& c) -> std::uint32_t { return c.battery_gauge_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.battery_gauge_enabled = (v != 0); }),
    bool_row("startup-arpeggio",  "boot_arp",   ApplyKind::Both,
             [](const DeviceConfig& c) -> std::uint32_t { return c.startup_arpeggio_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.startup_arpeggio_enabled = (v != 0); }),
    bool_row("servo-enabled",     "srv_en",     ApplyKind::Staged,
             [](const DeviceConfig& c) -> std::uint32_t { return c.servo_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.servo_enabled = (v != 0); }),
    bool_row("led-mouth-sync",    "led_msync",  ApplyKind::Both,
             [](const DeviceConfig& c) -> std::uint32_t { return c.led_mouth_sync_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.led_mouth_sync_enabled = (v != 0); }),
    bool_row("mic-lip-agc",       "ml_agc",     ApplyKind::Both,
             [](const DeviceConfig& c) -> std::uint32_t { return c.mic_lip_agc_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.mic_lip_agc_enabled = (v != 0); }),
    bool_row("barge-in",          "bargein_en", ApplyKind::Both,
             [](const DeviceConfig& c) -> std::uint32_t { return c.barge_in_enabled ? 1 : 0; },
             [](DeviceConfig& c, std::uint32_t v) { c.barge_in_enabled = (v != 0); }),
    // --- enums (u8 on the wire, bounds-checked, out-of-range keeps default) -
    num_row("provider",       "provider",   ValueType::U8, ApplyKind::Staged,
            static_cast<std::uint32_t>(Provider::XiaoZhi), false,
            [](const DeviceConfig& c) -> std::uint32_t { return static_cast<std::uint32_t>(c.provider); },
            [](DeviceConfig& c, std::uint32_t v) { c.provider = static_cast<Provider>(v); }),
    num_row("operation-mode", "op_mode",    ValueType::U8, ApplyKind::Staged,
            static_cast<std::uint32_t>(OperationMode::AsrLocal), false,
            [](const DeviceConfig& c) -> std::uint32_t { return static_cast<std::uint32_t>(c.operation_mode); },
            [](DeviceConfig& c, std::uint32_t v) { c.operation_mode = static_cast<OperationMode>(v); }),
    num_row("audio-output",   "audio_out",  ValueType::U8, ApplyKind::Staged,
            static_cast<std::uint32_t>(AudioOutput::ModuleAudio), false,
            [](const DeviceConfig& c) -> std::uint32_t { return static_cast<std::uint32_t>(c.audio_output); },
            [](DeviceConfig& c, std::uint32_t v) { c.audio_output = static_cast<AudioOutput>(v); }),
    num_row("lip-sync-mode",  "lipsync_md", ValueType::U8, ApplyKind::Both,
            static_cast<std::uint32_t>(LipSyncMode::LevelMeter), false,
            [](const DeviceConfig& c) -> std::uint32_t { return static_cast<std::uint32_t>(c.lip_sync_mode); },
            [](DeviceConfig& c, std::uint32_t v) { c.lip_sync_mode = static_cast<LipSyncMode>(v); }),
    // --- live-applied numerics (single-writer NVS saves, not in full save) --
    num_row("led-mode",       "led_mode",   ValueType::U8, ApplyKind::Live, 0, false,
            [](const DeviceConfig& c) -> std::uint32_t { return c.led_mode; },
            [](DeviceConfig& c, std::uint32_t v) { c.led_mode = static_cast<std::uint8_t>(v); }),
    num_row("led-color",      "led_color",  ValueType::U32, ApplyKind::Live, 0, false,
            [](const DeviceConfig& c) -> std::uint32_t { return c.led_color; },
            [](DeviceConfig& c, std::uint32_t v) { c.led_color = v; }),
    num_row("led-brightness", "led_bright", ValueType::U8, ApplyKind::Live, 0, false,
            [](const DeviceConfig& c) -> std::uint32_t { return c.led_brightness; },
            [](DeviceConfig& c, std::uint32_t v) { c.led_brightness = static_cast<std::uint8_t>(v); }),
    num_row("led-gradient-period", "led_period", ValueType::U8, ApplyKind::Live, 0, false,
            [](const DeviceConfig& c) -> std::uint32_t { return c.led_gradient_period_ds; },
            [](DeviceConfig& c, std::uint32_t v) { c.led_gradient_period_ds = static_cast<std::uint8_t>(v); }),
    num_row("mic-lip-input-gain",  "mic_lip_in",  ValueType::U16, ApplyKind::Live, 0, false,
            [](const DeviceConfig& c) -> std::uint32_t { return c.mic_lip_input_gain_pct; },
            [](DeviceConfig& c, std::uint32_t v) { c.mic_lip_input_gain_pct = static_cast<std::uint16_t>(v); }),
    num_row("mic-lip-output-gain", "mic_lip_out", ValueType::U16, ApplyKind::Live, 0, false,
            [](const DeviceConfig& c) -> std::uint32_t { return c.mic_lip_output_gain_pct; },
            [](DeviceConfig& c, std::uint32_t v) { c.mic_lip_output_gain_pct = static_cast<std::uint16_t>(v); }),
    num_row("speaker-volume", "spk_vol_pct", ValueType::U16, ApplyKind::Live, 200, true,
            [](const DeviceConfig& c) -> std::uint32_t { return c.speaker_volume_pct; },
            [](DeviceConfig& c, std::uint32_t v) { c.speaker_volume_pct = static_cast<std::uint16_t>(v); }),
}};

} // namespace

const std::array<SettingDescriptor, kSettingCount>& table()
{
    return kTable;
}

const SettingDescriptor* find(std::string_view id)
{
    for (const auto& d : kTable) {
        if (id == d.id) return &d;
    }
    return nullptr;
}

const SettingDescriptor* find_by_nvs_key(std::string_view nvs_key)
{
    for (const auto& d : kTable) {
        if (nvs_key == d.nvs_key) return &d;
    }
    return nullptr;
}

} // namespace stackchan::config::registry
