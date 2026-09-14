// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Stack-chan Recovery — ADR-001 の固定領域 Recovery アプリ。
// やることは 3 つだけ: NVS から Wi-Fi / 認証設定を読む、BLE と HTTP で
// OTA を受け付ける、release-fetch で GitHub Pages から Main を取ってくる。

#include <cstdio>
#include <memory>
#include <string>

#include <esp_app_desc.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <config_service/ota.hpp>
#include <wifi_config_service/release_ota.hpp>
#include <flash_layout/flash_layout.hpp>

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

// 受信開始前に失敗したとき: 旧 Main は無傷なので Main に戻して再起動する
// (ADR-001「更新開始と電断復旧」)。
void return_to_main_and_restart(const char* why)
{
    ESP_LOGW(kTag, "%s — returning to main", why);
    if (auto r = stackchan::flash_layout::return_to_main(); !r) {
        ESP_LOGE(kTag, "return_to_main: %s (staying in recovery)", stackchan::flash_layout::error_name(r.error()));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// bootctl の request_tag に従って release-fetch を自動で走らせる。
//   - Wi-Fi が 60 s 以内に繋がらなければ Main へ戻す。
//   - 取得開始に失敗 / 1 バイトも受信せずに終わったら Main へ戻す。
//   - 受信を始めた後の失敗は Recovery に留まる (Main は消去済み)。
//   - 成功すれば ota.cpp の end で arm_main → 再起動。
void auto_fetch_task(void* arg)
{
    std::unique_ptr<std::string> tag(static_cast<std::string*>(arg));
    ESP_LOGI(kTag, "auto release-fetch requested: tag=%s", tag->c_str());
    for (int i = 0; i < 60 && !stackchan::recovery::wifi::connected(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!stackchan::recovery::wifi::connected()) {
        return_to_main_and_restart("Wi-Fi not connected within 60 s");
        vTaskDelete(nullptr);
    }
    auto r = stackchan::wifi_config::release_ota::start(*tag, static_cast<std::uint8_t>(CONFIG_STACKCHAN_RECOVERY_BOARD_KIND));
    if (!r) {
        return_to_main_and_restart("release-fetch start failed");
        vTaskDelete(nullptr);
    }
    // ワーカーの終了を待つ。成功時はワーカーが再起動をスケジュールするので戻ってこない。
    while (stackchan::wifi_config::release_ota::active()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));  // end → 500 ms 後の再起動を待つ
    const std::string st = stackchan::config::ota::status_json();
    if (st.find("\"received\":0,") != std::string::npos) {
        return_to_main_and_restart("release-fetch failed before receiving");
    } else {
        ESP_LOGW(kTag, "release-fetch failed after receive started — staying in recovery: %s", st.c_str());
    }
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void app_main()
{
    const esp_app_desc_t* desc = esp_app_get_description();
    ESP_LOGI(kTag, "Stack-chan Recovery %s (board kind %d)",
             desc != nullptr ? desc->version : "?", CONFIG_STACKCHAN_RECOVERY_BOARD_KIND);

    // ADR-001 の拡張テーブル / bootctl。NVS 初期化より前に呼ぶ: 標準テーブルに
    // recovery が無い (Main 開発用テーブル) とき、自分の領域を登録してからでないと
    // NVS 書き込みが esp_ota_get_running_partition() で abort する。
    // 暫定表 (exttab 無し) では失敗するが、その場合は従来どおり標準テーブルの
    // ota_0 に書く (ota.cpp の既定動作)。
    std::string request_tag;
    if (auto r = stackchan::flash_layout::init(); r) {
        ESP_LOGI(kTag, "exttab gen=%lu entries=%u", static_cast<unsigned long>(r->generation), r->entry_count);
        const esp_partition_t* main_part = stackchan::flash_layout::find("main");
        if (main_part == nullptr) {
            ESP_LOGE(kTag, "exttab has no 'main' — cannot update");
        }
        stackchan::config::ota::set_target_partition(main_part);
        // 書き込み完了 → bootctl を target=Main, pending=1 にして再起動 (ota.cpp が行う)。
        stackchan::config::ota::set_finalize_hook([](const esp_partition_t*) -> esp_err_t {
            auto a = stackchan::flash_layout::arm_main();
            if (!a) {
                ESP_LOGE(kTag, "arm_main: %s", stackchan::flash_layout::error_name(a.error()));
                return ESP_FAIL;
            }
            return ESP_OK;
        });
        if (auto b = stackchan::flash_layout::read_bootctl(); b) {
            ESP_LOGI(kTag, "bootctl target=%u pending=%u attempts=%u tag='%s'", b->target, b->pending,
                     b->attempts, b->request_tag);
            request_tag = b->request_tag;
        }
    } else {
        ESP_LOGW(kTag, "flash_layout: %s — legacy partition table", stackchan::flash_layout::error_name(r.error()));
    }

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

    if (!request_tag.empty()) {
        // 12 KiB 内部 RAM: release_ota::start 自体は軽いが、状態ポーリングと
        // return_to_main の flash 書き込みを行うので PSRAM スタックは不可。
        xTaskCreate(&auto_fetch_task, "rcv-auto", 4096, new std::string(request_tag), tskIDLE_PRIORITY + 2,
                    nullptr);
    }
}
