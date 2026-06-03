// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <wifi_config_service/wifi_config_service.hpp>

#include "http_handlers.hpp"

#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mdns.h>

namespace stackchan::wifi_config {

namespace {

constexpr const char* kTag = "cfg-wifi";

bool g_started = false;
httpd_handle_t g_server = nullptr;
char g_hostname[32] = "stackchan";

// Build the mDNS hostname from the Wi-Fi STA MAC lower 3 bytes. The BLE
// advertising name (config_service) is composed from the SAME MAC, so a
// central can derive the .local hostname from the BLE name (tools/settings.html
// does exactly that). STA is the ESP32 base MAC and is what the router shows,
// so the hostname suffix matches the device's visible Wi-Fi MAC. (Both sides
// MUST stay on ESP_MAC_WIFI_STA — they previously diverged, BLE on ESP_MAC_BT
// = base + 2, which made the settings-page link unresolvable.)
void compose_hostname()
{
    std::uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(g_hostname, sizeof(g_hostname),
                  "stackchan-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

tl::expected<void, Error> start_mdns()
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "mdns_init: %s", esp_err_to_name(err));
        return tl::unexpected(Error::MdnsInit);
    }

    mdns_hostname_set(g_hostname);
    mdns_instance_name_set("Stack-chan config");

    // Plain HTTP on 80. TLS turned out too heavy on internal RAM (each
    // active TLS session was contending with the conversation task's
    // mbedtls / Wi-Fi AES DMA allocations). The settings page lives on
    // the home LAN so the trust model is "anyone on the LAN can talk to
    // it" — same as a router admin page.
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    mdns_service_add(nullptr, "_stackchan-config", "_tcp", 80, nullptr, 0);

    // Live audio receiver (main/wifi_audio.cpp): RTP on udp/6970. Advertise
    // the port + the wire format so a sender can discover where/how to push.
    // Keep the port in sync with kPort in main/wifi_audio.cpp.
    mdns_service_add(nullptr, "_stackchan-audio", "_udp", 6970, nullptr, 0);
    // udp/6970 carries L16 or μ-law (codec from RTP PT: 0 → PCMU/8000 G.711
    // μ-law e.g. OBS, else → L16/48000 ffmpeg/gst). AAC (MPEG4-GENERIC) shares
    // the dynamic PT with L16, so it has its own port (6972), surfaced here.
    mdns_txt_item_t audio_txt[] = {
        {"proto", "rtp"},
        {"codec", "L16,PCMU"},
        {"ch", "1"},
        {"aac_port", "6972"},
    };
    mdns_service_txt_set("_stackchan-audio", "_udp", audio_txt,
                         sizeof(audio_txt) / sizeof(audio_txt[0]));

    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc != nullptr) {
        mdns_txt_item_t txt[] = {
            {"version", desc->version},
            {"path", "/"},
        };
        mdns_service_txt_set("_http", "_tcp", txt, sizeof(txt) / sizeof(txt[0]));
        mdns_service_txt_set("_stackchan-config", "_tcp", txt, sizeof(txt) / sizeof(txt[0]));
    }

    ESP_LOGI(kTag, "mDNS published as %s.local", g_hostname);
    return {};
}

tl::expected<httpd_handle_t, Error> start_http_server()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    // Wide enough to hold all the /api/* routes (currently ~16) plus a few
    // for future growth without bumping again.
    cfg.max_uri_handlers = 24;
    // Stack MUST live in internal RAM: the OTA control handler calls
    // esp_ota_begin → esp_partition_mmap, which disables CPU cache while
    // remapping flash. PSRAM is cached, so a PSRAM-resident stack becomes
    // unreadable during the cache-disable window and the kernel asserts
    // (cache_utils.c:152 esp_task_stack_is_sane_cache_disabled). Same
    // constraint applies to NVS writes triggered by /api/apply. 6 KiB is
    // enough for cJSON + esp_ota_write + the few hundred bytes of mbedtls
    // state nothing actually uses here (no TLS in plain HTTP mode).
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    // Settings page is single-user.
    cfg.max_open_sockets = 1;

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start: %s", esp_err_to_name(err));
        return tl::unexpected(Error::HttpServerStart);
    }
    ESP_LOGI(kTag, "HTTP server listening on :%d", cfg.server_port);
    return server;
}

// Done on a worker task because mDNS init pulls in a few KB of stack we
// don't want eating into the sys_evt task. Even though plain HTTP is much
// lighter than TLS, this keeps the structure consistent and gives us
// margin for future additions.
void init_task(void* arg)
{
    const auto* current = static_cast<const config::DeviceConfig*>(arg);

    if (auto r = start_mdns(); !r) {
        ESP_LOGE(kTag, "start_mdns failed: %d", static_cast<int>(r.error()));
        vTaskDelete(nullptr);
        return;
    }

    auto server = start_http_server();
    if (!server) {
        ESP_LOGE(kTag, "start_http_server failed: %d", static_cast<int>(server.error()));
        vTaskDelete(nullptr);
        return;
    }
    g_server = *server;
    http::register_handlers(g_server, *current);

    vTaskDelete(nullptr);
}

} // namespace

tl::expected<void, Error> start(const config::DeviceConfig& current)
{
    if (g_started) {
        return tl::unexpected(Error::AlreadyStarted);
    }

    compose_hostname();

    g_started = true;
    BaseType_t ok = xTaskCreate(init_task, "cfg-wifi-init", 6 * 1024,
                                const_cast<void*>(static_cast<const void*>(&current)),
                                tskIDLE_PRIORITY + 3, nullptr);
    if (ok != pdPASS) {
        g_started = false;
        ESP_LOGE(kTag, "xTaskCreate(cfg-wifi-init) failed");
        return tl::unexpected(Error::HttpServerStart);
    }
    return {};
}

void notify_wifi_connected(bool connected)
{
    if (!g_started) return;
    http::set_wifi_connected(connected);
}

void set_battery(int millivolts, int milliamps, int percent)
{
    if (!g_started) return;
    http::set_battery(millivolts, milliamps, percent);
}

void set_servo_range_mode_sink(config::ServoRangeModeSink sink)
{
    http::set_servo_range_mode_sink(sink);
}

void set_servo_positions_getter(config::ServoPositionsGetter getter)
{
    http::set_servo_positions_getter(getter);
}

void set_board_kind(std::uint8_t kind)
{
    http::set_board_kind(kind);
}

void set_avatar_bytecode_sink(AvatarBytecodeSink sink)
{
    http::set_avatar_bytecode_sink(std::move(sink));
}

} // namespace stackchan::wifi_config
