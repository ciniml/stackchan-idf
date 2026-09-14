// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Stack-chan Recovery — ADR-001 の固定領域 Recovery アプリ。
// やることは 3 つだけ: NVS から Wi-Fi / 認証設定を読む、BLE と HTTP で
// OTA を受け付ける、release-fetch で GitHub Pages から Main を取ってくる。

#include <cstdio>
#include <string>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <config_service/ota.hpp>

#include "ble.hpp"
#include "http.hpp"
#include "wifi.hpp"

namespace {

constexpr const char* kTag = "recovery";

// Main の config_store と同じ namespace / キー (settings_registry.cpp)。
constexpr const char* kNvsNamespace = "stackchan_cfg";

std::string nvs_read_str(nvs_handle_t h, const char* key)
{
    std::size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return {};
    std::string out(len, '\0');
    if (nvs_get_str(h, key, out.data(), &len) != ESP_OK) return {};
    out.resize(len > 0 ? len - 1 : 0); // NUL を落とす
    return out;
}

struct Settings {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string auth_password;
    std::string device_name;
};

Settings load_settings()
{
    Settings s;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(kTag, "NVS namespace %s not found — BLE only, no auth", kNvsNamespace);
        return s;
    }
    s.wifi_ssid = nvs_read_str(h, "wifi_ssid");
    s.wifi_password = nvs_read_str(h, "wifi_pass");
    s.auth_password = nvs_read_str(h, "auth_pwd");
    s.device_name = nvs_read_str(h, "dev_name");
    nvs_close(h);
    return s;
}

} // namespace

extern "C" void app_main()
{
    const esp_app_desc_t* desc = esp_app_get_description();
    ESP_LOGI(kTag, "Stack-chan Recovery %s (board kind %d)",
             desc != nullptr ? desc->version : "?", CONFIG_STACKCHAN_RECOVERY_BOARD_KIND);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 設定を失う操作だが、NVS が読めない状態では Wi-Fi 認証情報も使えず
        // Recovery の目的 (更新受付) を果たせないので初期化する。
        ESP_LOGW(kTag, "NVS unusable (%s) — erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const Settings s = load_settings();

    // Main のイメージ (project "stackchan_idf") だけを受け入れる。
    stackchan::config::ota::set_expected_project_name("stackchan_idf");

    stackchan::recovery::ble::start(s.device_name, s.auth_password);
    stackchan::recovery::wifi::start(s.wifi_ssid, s.wifi_password);
    stackchan::recovery::http::start(s.auth_password,
                                     static_cast<std::uint8_t>(CONFIG_STACKCHAN_RECOVERY_BOARD_KIND));
}
