// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "wifi_sta.hpp"

#include <atomic>
#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <M5Unified.h>

#include <config_service/config_service.hpp>
#include <wifi_config_service/wifi_config_service.hpp>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "wifi-sta";

std::atomic<bool> g_connected{false};
// Snapshot of the config used at boot — held so the HTTP settings service
// can be brought up on the *first* IP_EVENT_STA_GOT_IP with the same view
// of NVS that the BLE service registered. After the first start it stays
// up across reconnects (wifi_config_service::start ignores re-entry).
const config::DeviceConfig* g_boot_cfg = nullptr;

void event_handler(void* /*arg*/, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            g_connected.store(false, std::memory_order_release);
            config::notify_wifi_connected(false);
            wifi_config::notify_wifi_connected(false);
            ESP_LOGW(kTag, "disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(kTag, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        g_connected.store(true, std::memory_order_release);
        config::notify_wifi_connected(true);

        // Bring up mDNS + HTTP settings server on the first successful STA
        // association. Subsequent reconnects are no-ops inside start().
        if (g_boot_cfg != nullptr) {
            if (auto r = wifi_config::start(*g_boot_cfg); !r) {
                if (r.error() != wifi_config::Error::AlreadyStarted) {
                    ESP_LOGW(kTag, "wifi_config::start failed: %d",
                             static_cast<int>(r.error()));
                }
            }
        }
        wifi_config::notify_wifi_connected(true);

        // SNTP — start once on the first connection. esp_netif_sntp_init
        // sets up the lwIP SNTP client in background; it will write the
        // system time as soon as a response comes back. We also push the
        // synced time into M5.Rtc once it's valid so the watch keeps
        // sensible time across short power loss (RX8130CE is battery-
        // backed via the M5PM1 PMIC). JST is hardcoded — no UI to pick
        // a TZ on a wearable, and the dev unit lives in JP.
        static bool sntp_started = false;
        if (!sntp_started) {
            sntp_started = true;
            setenv("TZ", "JST-9", 1);
            tzset();
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            cfg.start = true;
            esp_netif_sntp_init(&cfg);
            // Mirror system time into M5.Rtc once SNTP has converged.
            // Block briefly here to give the first sync a chance; if it
            // hasn't arrived in 5 s we move on and the next reconnect
            // will retry. ESP_OK = synced, ERR_TIMEOUT = not yet.
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000)) == ESP_OK) {
                time_t now = 0;
                ::time(&now);
                struct tm tm_now{};
                localtime_r(&now, &tm_now);
                m5::rtc_datetime_t dt{};
                dt.date.year    = tm_now.tm_year + 1900;
                dt.date.month   = tm_now.tm_mon + 1;
                dt.date.date    = tm_now.tm_mday;
                dt.date.weekDay = tm_now.tm_wday;
                dt.time.hours   = tm_now.tm_hour;
                dt.time.minutes = tm_now.tm_min;
                dt.time.seconds = tm_now.tm_sec;
                M5.Rtc.setDateTime(dt);
                ESP_LOGI(kTag, "SNTP synced → RTC set %04d-%02d-%02d %02d:%02d:%02d JST",
                         dt.date.year, dt.date.month, dt.date.date,
                         dt.time.hours, dt.time.minutes, dt.time.seconds);
            } else {
                ESP_LOGW(kTag, "SNTP sync timeout (will keep trying in background)");
            }
        }
    }
}

} // namespace

void wifi_start(const config::DeviceConfig& cfg)
{
    g_boot_cfg = &cfg;
    ESP_ERROR_CHECK(esp_netif_init());

    static bool loop_started = false;
    if (!loop_started) {
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        loop_started = true;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, nullptr));

    if (cfg.wifi_ssid.empty()) {
        ESP_LOGI(kTag, "no SSID configured — Wi-Fi driver initialised but not started");
        return;
    }

    wifi_config_t sta_cfg{};
    std::strncpy(reinterpret_cast<char*>(sta_cfg.sta.ssid),
                 cfg.wifi_ssid.c_str(), sizeof(sta_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta_cfg.sta.password),
                 cfg.wifi_password.c_str(), sizeof(sta_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(kTag, "connecting to SSID: %s", cfg.wifi_ssid.c_str());
}

bool wifi_is_connected()
{
    return g_connected.load(std::memory_order_acquire);
}

} // namespace stackchan::app
