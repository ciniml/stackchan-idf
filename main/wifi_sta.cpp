// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "wifi_sta.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <M5Unified.h>

#include <config_service/config_service.hpp>
#include <wifi_config_service/wifi_config_service.hpp>

#include "captive_portal.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "wifi-sta";

std::atomic<bool> g_connected{false};
std::atomic<bool> g_ap_active{false};
// Cached AP credentials so wifi_ap_info() can serve the on-device QR screen
// without re-querying the driver. Derived from ESP_MAC_WIFI_STA.
char g_ap_ssid[33] = {0};
char g_ap_pw[33]   = {0};
char g_ap_ip[16]   = "192.168.4.1";
esp_netif_t* g_ap_netif = nullptr;
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

// --- SoftAP provisioning ----------------------------------------------------
//
// Triggered from the on-device Settings UI when home Wi-Fi credentials are
// missing/wrong. Brings up an isolated AP on top of the existing STA driver
// (APSTA dual mode) so the user can join the AP from an iPhone — which has
// no Web Bluetooth — and configure home Wi-Fi through the same settings_wifi
// HTTP page used over STA. STA stays attempting reconnect in parallel; we
// intentionally do not tear the AP down on STA_GOT_IP because the user is
// often mid-edit when their changes take effect.
//
// Credentials are derived from the STA MAC (same low 3 bytes already used
// for the BLE advertising name and mDNS hostname) so they are stable across
// reboots and printed/displayed on the QR screen:
//   SSID = "Stackchan-AABBCC"  (6 hex chars)
//   PSK  = "sc-AABBCCDD"       (8 hex chars; 11 char total; WPA2 minimum 8)

namespace {

void derive_ap_credentials()
{
    if (g_ap_ssid[0] != 0) return; // already derived
    std::uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(g_ap_ssid, sizeof(g_ap_ssid),
                  "Stackchan-%02X%02X%02X", mac[3], mac[4], mac[5]);
    std::snprintf(g_ap_pw, sizeof(g_ap_pw),
                  "sc-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
}

} // namespace

// Worker task body — runs the parts of AP bring-up that may take O(seconds)
// (HTTP server start + handle polling) off the caller's task so a tap
// handler in demo_loop returns quickly and the QR screen appears within
// one render frame.
void ap_post_start_worker(void*)
{
    if (g_boot_cfg != nullptr) {
        // wifi_config::start is idempotent — returns AlreadyStarted if the
        // server is already up from a previous STA_GOT_IP. The server
        // binds to INADDR_ANY so it serves both AP and STA interfaces.
        (void)wifi_config::start(*g_boot_cfg);
    }
    wifi_config::set_provisioning_mode(true);
    captive_portal::start_dns();
    // Poll up to ~2 s for the init task to publish its server handle.
    // Done off the caller so demo_loop isn't blocked.
    for (int i = 0; i < 40; ++i) {
        if (auto h = wifi_config::handle(); h != nullptr) {
            captive_portal::register_http_catchall(h);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(nullptr);
}

void wifi_enable_ap_mode()
{
    if (g_ap_active.load(std::memory_order_acquire)) return;

    derive_ap_credentials();
    if (g_ap_netif == nullptr) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
    }

    // APSTA keeps the STA side of the driver running so reconnect attempts
    // continue in background; the user can fix credentials and watch them
    // take effect without rebooting.
    //
    // Use error-returns instead of ESP_ERROR_CHECK throughout: set_mode can
    // legitimately fail when STA is mid-reconnect-storm (wrong password
    // configured → tight WIFI_EVENT_STA_DISCONNECTED → esp_wifi_connect()
    // loop) and aborting the entire firmware on a transient set_mode error
    // looks to the user like "tap did nothing" (the device just reboots).
    if (esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA); err != ESP_OK) {
        ESP_LOGE(kTag, "set_mode(APSTA) failed: %s — AP not enabled",
                 esp_err_to_name(err));
        return;
    }

    wifi_config_t ap_cfg{};
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), g_ap_ssid,
                 sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = std::strlen(g_ap_ssid);
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), g_ap_pw,
                 sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.channel        = 6;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.pmf_cfg.required = false;
    if (esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg); err != ESP_OK) {
        ESP_LOGE(kTag, "set_config(AP) failed: %s — reverting to STA",
                 esp_err_to_name(err));
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }

    // esp_wifi_start() is idempotent; calling it after set_mode picks up the
    // new AP interface even when STA was already running.
    esp_err_t st = esp_wifi_start();
    if (st != ESP_OK && st != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(kTag, "esp_wifi_start (AP) returned %s", esp_err_to_name(st));
    }

    // Read the AP IP back so the QR screen reflects whatever the driver
    // chose (default 192.168.4.1 unless reconfigured elsewhere).
    if (g_ap_netif != nullptr) {
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(g_ap_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            std::snprintf(g_ap_ip, sizeof(g_ap_ip), IPSTR, IP2STR(&ip.ip));
        }
    }

    g_ap_active.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "AP up: SSID=\"%s\" PW=\"%s\" IP=%s",
             g_ap_ssid, g_ap_pw, g_ap_ip);

    // Hand off the rest (HTTP server / mDNS / captive portal) to a worker
    // task. demo_loop returns immediately so the next render-task tick
    // sees ap_active() = true and ap_screen paints. Stack 3 KiB internal
    // RAM — flash-safe (no OTA / NVS writes happen here).
    xTaskCreate(&ap_post_start_worker, "ap-post-start", 3 * 1024, nullptr,
                tskIDLE_PRIORITY + 2, nullptr);
}

void wifi_disable_ap_mode()
{
    if (!g_ap_active.load(std::memory_order_acquire)) return;
    // Tear down captive portal first so STA-mode clients don't see the
    // 404-to-root redirect anymore (the catch-all is harmless over STA,
    // but it'd hijack legitimate 404s from poorly-written clients).
    captive_portal::stop_dns();
    if (auto h = wifi_config::handle(); h != nullptr) {
        captive_portal::unregister_http_catchall(h);
    }
    wifi_config::set_provisioning_mode(false);
    // Non-aborting: a failed set_mode here just leaves the AP up, which is
    // recoverable (user can tap again). An ESP_ERROR_CHECK abort would
    // mid-reboot the device with no warning.
    if (esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA); err != ESP_OK) {
        ESP_LOGW(kTag, "set_mode(STA) on AP-disable failed: %s",
                 esp_err_to_name(err));
    }
    g_ap_active.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "AP down");
}

bool wifi_ap_active()
{
    return g_ap_active.load(std::memory_order_acquire);
}

bool wifi_ap_info(char* ssid, std::size_t ssid_cap,
                  char* password, std::size_t pw_cap,
                  char* ip, std::size_t ip_cap)
{
    if (!g_ap_active.load(std::memory_order_acquire)) return false;
    if (ssid == nullptr || password == nullptr || ip == nullptr) return false;
    if (ssid_cap <= std::strlen(g_ap_ssid)) return false;
    if (pw_cap   <= std::strlen(g_ap_pw))   return false;
    if (ip_cap   <= std::strlen(g_ap_ip))   return false;
    std::strcpy(ssid, g_ap_ssid);
    std::strcpy(password, g_ap_pw);
    std::strcpy(ip, g_ap_ip);
    return true;
}

} // namespace stackchan::app
