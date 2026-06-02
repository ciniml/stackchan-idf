// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "atom_status.hpp"

#include <atomic>
#include <cstdio>
#include <string>

#include <M5Unified.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>

#include <config_service/config_service.hpp>
#include <config_service/config_store.hpp>

#include "wifi_sta.hpp"

namespace stackchan::app::atom_status {

namespace {

SharedState* g_state = nullptr;
std::atomic<bool> g_active{false};
std::string g_ssid;

std::uint32_t now_ms_local()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

std::uint32_t g_last_draw_ms = 0;

std::string current_ip()
{
    esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    if (nif != nullptr && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr != 0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
        return std::string(buf);
    }
    return "-";
}

const char* conv_status_label(ConvStatus s)
{
    switch (s) {
    case ConvStatus::Disabled:     return "off";
    case ConvStatus::WaitingWifi:  return "wifi?";
    case ConvStatus::Connecting:   return "conn..";
    case ConvStatus::Listening:    return "ready";
    case ConvStatus::Talking:      return "talk";
    case ConvStatus::Yielded:      return "yld";
    case ConvStatus::Reconnecting: return "recn..";
    case ConvStatus::Error:        return "err";
    }
    return "?";
}

} // namespace

void init(SharedState& state)
{
    g_state = &state;
    const config::DeviceConfig cfg = config::load();
    g_ssid = cfg.wifi_ssid;
}

void poll_button()
{
    // M5.update() is called by demo_loop on each iteration; we just look at
    // the latched press state. Short press = toggle the overlay.
    if (M5.BtnA.wasPressed()) {
        g_active.store(!g_active.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    }
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

bool draw(avatar::RichCanvas& canvas)
{
    // Refresh ~2 Hz so IP / Wi-Fi state stay current without burning CPU on
    // every render-task tick.
    const std::uint32_t t = now_ms_local();
    if (t - g_last_draw_ms < 500) return false;
    g_last_draw_ms = t;

    const std::int32_t w = canvas.width();
    const std::int32_t h = canvas.height();

    const std::uint16_t bg   = canvas.color565(20, 22, 28);
    const std::uint16_t fg   = canvas.color565(235, 235, 235);
    const std::uint16_t dim  = canvas.color565(150, 150, 150);
    const std::uint16_t ok   = canvas.color565(80, 220, 120);
    const std::uint16_t off  = canvas.color565(230, 110, 110);

    canvas.request_full_repaint();
    canvas.begin_frame(bg);
    canvas.fillRect(0, 0, w, h, bg);

    const esp_app_desc_t* app = esp_app_get_description();
    const bool wifi = wifi_is_connected();
    const bool ble = config::ble_connected();
    const std::string ip = current_ip();
    const ConvStatus cs = g_state ? g_state->conversation_status.load(std::memory_order_relaxed)
                                  : ConvStatus::Disabled;

    canvas.setFont(&fonts::lgfxJapanGothic_12);
    canvas.setTextDatum(lgfx::textdatum_t::top_left);

    auto kv = [&](int row, const char* key, const char* value, std::uint16_t vcolor) {
        const int y = 6 + row * 14;
        canvas.setTextColor(dim);
        canvas.drawString(key, 4, y);
        canvas.setTextColor(vcolor);
        canvas.drawString(value, 44, y);
    };

    int row = 0;
    kv(row++, "FW",   app ? app->version : "?", fg);
    kv(row++, "SSID", g_ssid.empty() ? "(unset)" : g_ssid.c_str(), fg);
    kv(row++, "IP",   ip.c_str(), wifi ? fg : off);
    kv(row++, "Wifi", wifi ? "up" : "down", wifi ? ok : off);
    kv(row++, "BLE",  ble ? "conn" : "wait", ble ? ok : fg);
    kv(row++, "Conv", conv_status_label(cs), fg);

    // Hint to press the button to dismiss.
    canvas.setTextColor(dim);
    canvas.setTextDatum(lgfx::textdatum_t::bottom_center);
    canvas.drawString("press BtnA", w / 2, h - 2);
    return true;
}

} // namespace stackchan::app::atom_status
