// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "wifi_provisioning.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "wifi-prov";
constexpr const char* kPop = "stackchan";          // Proof-of-Possession
constexpr const char* kServicePrefix = "PROV_SC_"; // BLE advertised name prefix

constexpr int kProvDoneBit = BIT0;

EventGroupHandle_t g_event_group = nullptr;
std::atomic<bool> g_connected{false};

void event_handler(void* /*arg*/, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
        case WIFI_PROV_START:
            ESP_LOGI(kTag, "provisioning started");
            break;
        case WIFI_PROV_CRED_RECV: {
            const auto* cred = static_cast<wifi_sta_config_t*>(data);
            ESP_LOGI(kTag, "creds received: ssid=%s", cred->ssid);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            const auto* reason = static_cast<wifi_prov_sta_fail_reason_t*>(data);
            ESP_LOGW(kTag, "provisioning failed: %s",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "auth-error" : "ap-not-found");
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(kTag, "provisioning credentials accepted");
            break;
        case WIFI_PROV_END:
            ESP_LOGI(kTag, "provisioning ended");
            xEventGroupSetBits(g_event_group, kProvDoneBit);
            break;
        default:
            break;
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            g_connected.store(false, std::memory_order_release);
            ESP_LOGW(kTag, "Wi-Fi disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(kTag, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        g_connected.store(true, std::memory_order_release);
    }
}

void get_service_name(char* out, std::size_t len)
{
    std::uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out, len, "%s%02X%02X%02X", kServicePrefix, mac[3], mac[4], mac[5]);
}

void draw_provisioning_ui(M5GFX& display, const char* service_name, const char* pop)
{
    // The "ESP BLE Provisioning" app expects a JSON payload encoded as a
    // QR code. See https://github.com/espressif/esp-idf-provisioning-android
    char payload[160];
    std::snprintf(payload, sizeof(payload),
                  "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
                  service_name, pop);

    display.fillScreen(TFT_WHITE);
    display.setTextColor(TFT_BLACK, TFT_WHITE);
    display.setTextDatum(lgfx::textdatum_t::top_left);
    display.setTextSize(1);
    display.setFont(&fonts::lgfxJapanGothic_16);
    display.setCursor(10, 6);
    display.printf("BLE: %s", service_name);
    display.setCursor(10, 26);
    display.printf("PoP: %s", pop);

    constexpr int qr_size = 180;
    display.qrcode(payload, (display.width() - qr_size) / 2 + 60, 45, qr_size, 6);

    display.setCursor(10, 224);
    display.print("Scan with ESP BLE Provisioning");
}

} // namespace

bool wifi_connect_or_provision(M5GFX& display)
{
    if (g_event_group == nullptr) {
        g_event_group = xEventGroupCreate();
    }

    if (nvs_flash_init() == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    static bool loop_started = false;
    if (!loop_started) {
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        loop_started = true;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, nullptr));

    wifi_prov_mgr_config_t mgr_cfg{};
    mgr_cfg.scheme = wifi_prov_scheme_ble;
    mgr_cfg.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
    ESP_ERROR_CHECK(wifi_prov_mgr_init(mgr_cfg));

    bool provisioned = false;
    wifi_prov_mgr_is_provisioned(&provisioned);

    if (provisioned) {
        ESP_LOGI(kTag, "Wi-Fi credentials already in NVS — connecting");
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        char service_name[32];
        get_service_name(service_name, sizeof(service_name));

        // 128-bit service UUID used by the BLE GATT service.
        static const uint8_t service_uuid[16] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        wifi_prov_scheme_ble_set_service_uuid(const_cast<uint8_t*>(service_uuid));

        ESP_LOGI(kTag, "starting BLE provisioning: name=%s pop=%s", service_name, kPop);
        draw_provisioning_ui(display, service_name, kPop);

        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
            WIFI_PROV_SECURITY_1, kPop, service_name, /*service_key=*/nullptr));

        ESP_LOGI(kTag, "waiting for provisioning to finish");
        xEventGroupWaitBits(g_event_group, kProvDoneBit, pdFALSE, pdTRUE,
                            portMAX_DELAY);
        wifi_prov_mgr_deinit();
    }

    // We don't block on IP here — the caller can poll wifi_is_connected().
    return true;
}

bool wifi_is_connected()
{
    return g_connected.load(std::memory_order_acquire);
}

} // namespace stackchan::app
