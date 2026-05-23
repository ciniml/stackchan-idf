// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "render_task.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <config_service/config_service.hpp>

#include "avatar/avatar.hpp"
#include "wifi_sta.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "render";
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(33);

struct InfoStatus {
    std::string version;
    bool wifi = false;
    bool ble = false;
    char ip[16] = "-";

    bool operator==(const InfoStatus& o) const
    {
        return wifi == o.wifi && ble == o.ble && version == o.version &&
               std::strcmp(ip, o.ip) == 0;
    }
};

InfoStatus gather_status()
{
    InfoStatus s;
    const esp_app_desc_t* app = esp_app_get_description();
    s.version = (app != nullptr) ? app->version : "?";
    s.wifi = wifi_is_connected();
    s.ble = config::ble_connected();
    esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info{};
    if (nif != nullptr && esp_netif_get_ip_info(nif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        std::snprintf(s.ip, sizeof(s.ip), IPSTR, IP2STR(&ip_info.ip));
    }
    return s;
}

// Draw the on-device info screen (version + Wi-Fi / BLE status) with a close
// button in the top-right corner. Drawn straight to the display only when the
// status changes, so there's no per-frame flicker.
void draw_info_screen(M5GFX& d, const InfoStatus& s)
{
    const int w = d.width();
    const std::uint16_t bg = d.color565(20, 22, 28);
    const std::uint16_t fg = d.color565(235, 235, 235);
    const std::uint16_t dim = d.color565(150, 150, 150);
    const std::uint16_t ok = d.color565(80, 220, 120);
    const std::uint16_t off = d.color565(230, 100, 100);

    d.fillScreen(bg);

    // Close button (top-right) — aligned with the demo_loop touch hit area.
    const int bs = 36;
    const int bx = w - bs - 8;
    const int by = 8;
    d.fillRoundRect(bx, by, bs, bs, 6, d.color565(70, 74, 84));
    d.setTextColor(fg);
    d.setTextSize(2);
    d.setTextDatum(lgfx::textdatum_t::middle_center);
    d.drawString("X", bx + bs / 2, by + bs / 2);

    // Title.
    d.setTextDatum(lgfx::textdatum_t::top_left);
    d.setTextColor(fg);
    d.setTextSize(3);
    d.drawString("Stack-chan", 14, 12);

    // Status rows.
    int y = 64;
    auto row = [&](const char* label, const char* value, std::uint16_t vcolor) {
        d.setTextSize(2);
        d.setTextColor(dim);
        d.drawString(label, 14, y);
        d.setTextColor(vcolor);
        d.drawString(value, 120, y);
        y += 30;
    };
    row("FW", s.version.c_str(), fg);
    row("Wi-Fi", s.wifi ? "connected" : "disconnected", s.wifi ? ok : off);
    row("IP", s.ip, fg);
    row("BLE", s.ble ? "connected" : "advertising", s.ble ? ok : dim);
}

void render_task_entry(void* arg)
{
    auto& args = *static_cast<RenderTaskArgs*>(arg);

    avatar::Avatar avatar{*args.display};
    if (!avatar.begin()) {
        ESP_LOGE(kTag, "avatar.begin() failed");
        vTaskDelete(nullptr);
        return;
    }

    int last_expression = -1;
    std::uint32_t last_balloon_version = 0;
    std::string balloon_scratch;
    bool balloon_pending = false;

    bool info_active = false;     // currently showing the info screen
    InfoStatus shown_status;      // last status drawn (for change detection)

    for (;;) {
        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);

        // --- On-device info screen ---
        if (args.state->info_screen.load(std::memory_order_relaxed)) {
            const InfoStatus status = gather_status();
            if (!info_active || !(status == shown_status)) {
                draw_info_screen(*args.display, status);
                shown_status = status;
            }
            info_active = true;
            vTaskDelay(kPeriodTicks);
            continue;
        }
        if (info_active) {
            // Returning to the avatar — its pushSprite() repaints the whole
            // screen on the next tick(), overwriting the info screen.
            info_active = false;
            last_expression = -1; // force a fresh expression apply
        }

        // --- Avatar ---
        const int expr = args.state->expression.load(std::memory_order_relaxed);
        if (expr != last_expression) {
            avatar.set_expression(static_cast<avatar::Expression>(expr));
            last_expression = expr;
        }
        avatar.set_mouth_open(args.state->mouth_open.load(std::memory_order_relaxed));

        const std::uint32_t balloon_version = args.state->balloon_version();
        if (balloon_version != last_balloon_version) {
            if (args.state->balloon_visible()) {
                std::uint32_t hold_ms = 0;
                args.state->snapshot_balloon(balloon_scratch, hold_ms);
                avatar.set_balloon_text(balloon_scratch, hold_ms);
                balloon_pending = true;
            } else {
                avatar.clear_balloon();
                balloon_pending = false;
            }
            last_balloon_version = balloon_version;
        }

        avatar.tick(now_ms);

        if (balloon_pending && avatar.is_balloon_done()) {
            balloon_pending = false;
            args.state->notify_balloon_complete();
        }

        // Use vTaskDelay (not vTaskDelayUntil) so the IDLE task on this core
        // always gets at least one tick, even if a frame ran long.
        vTaskDelay(kPeriodTicks);
    }
}

} // namespace

void start_render_task(RenderTaskArgs& args)
{
    xTaskCreatePinnedToCore(render_task_entry, "render", 8192, &args, 5, nullptr, 1);
}

} // namespace stackchan::app
