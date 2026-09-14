// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "wifi.hpp"

#include <atomic>
#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

namespace stackchan::recovery::wifi {

namespace {

constexpr const char* kTag = "rcv-wifi";

std::atomic<bool> g_connected{false};

// NUL 終端を保証した固定長コピー (esp_wifi の ssid/password 配列用)。
void copy_field(std::uint8_t* dst, std::size_t cap, const std::string& src)
{
    std::size_t n = src.size();
    if (n > cap - 1) n = cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

void event_handler(void* /*arg*/, esp_event_base_t base, std::int32_t id, void* data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            g_connected.store(false, std::memory_order_release);
            ESP_LOGW(kTag, "disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(kTag, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        g_connected.store(true, std::memory_order_release);
    }
}

} // namespace

void start(const std::string& ssid, const std::string& password)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (ssid.empty()) {
        ESP_LOGW(kTag, "no SSID stored — STA idle (BLE only)");
        ESP_ERROR_CHECK(esp_wifi_start());
        return;
    }

    wifi_config_t cfg{};
    copy_field(cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);
    copy_field(cfg.sta.password, sizeof(cfg.sta.password), password);
    cfg.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(kTag, "STA start: ssid=\"%s\"", ssid.c_str());
}

bool connected()
{
    return g_connected.load(std::memory_order_acquire);
}

} // namespace stackchan::recovery::wifi
