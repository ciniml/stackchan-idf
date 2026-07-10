// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "atom_status.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>

#include <M5Unified.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>

#include <config_service/config_service.hpp>
#include <config_service/config_store.hpp>

#include "ap_screen.hpp"
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

// Long-press cycle state. AtomS3R / AtomS3 have no on-screen settings UI,
// so we overload the single front button (M5.BtnA) with a duration-based
// gesture vocab:
//   - short tap                            → toggle the status overlay
//   - 1.5 s while overlay UP               → cycle operation_mode + reboot
//   - 1.5–5 s while overlay DOWN, released → toggle one-touch speaker mute
//     (fires on release, not at the 1.5 s mark, so holding on toward the
//      5 s AP threshold does not also flip the mute state)
//   - 5.0 s while overlay DOWN (idle)      → toggle SoftAP provisioning
//     (so an iOS user without home Wi-Fi can hold-press to bring up the
//      QR + AP without needing the overlay open first)
// Press-down sets the timestamp; each threshold edge-triggers exactly once
// while still held.
constexpr std::uint32_t kLongPressMs   = 1500;  // mode cycle (overlay up)
constexpr std::uint32_t kApPressMs     = 5000;  // AP toggle  (overlay down)
std::uint32_t g_press_start_ms = 0;
bool g_long_press_fired = false;
bool g_ap_press_fired   = false;

void cycle_operation_mode_and_reboot()
{
    config::DeviceConfig cfg = config::load();
    const std::uint8_t cur = static_cast<std::uint8_t>(cfg.operation_mode);
#if defined(CONFIG_STACKCHAN_ASR_ENABLED)
    const std::uint8_t next = (cur + 1) % 4;   // ASR (AsrLocal) を循環に含める
#else
    const std::uint8_t next = (cur + 1) % 3;
#endif
    cfg.operation_mode = static_cast<config::OperationMode>(next);
    (void)config::store::save(cfg);
    esp_restart();
}

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

const char* op_mode_short(config::OperationMode m)
{
    switch (m) {
    case config::OperationMode::MicLipSync:   return "mic";
    case config::OperationMode::JttsRandom:   return "jtts";
    case config::OperationMode::Conversation: return "conv";
    case config::OperationMode::AsrLocal:     return "asr";
    }
    return "?";
}

} // namespace

// Cached at init time so draw() doesn't have to re-read NVS every frame.
// Mode can only change via cycle_operation_mode_and_reboot (which reboots),
// so caching once is safe — there's no live-update path.
config::OperationMode g_op_mode = config::OperationMode::Conversation;

void init(SharedState& state)
{
    g_state = &state;
    const config::DeviceConfig cfg = config::load();
    g_ssid = cfg.wifi_ssid;
    g_op_mode = cfg.operation_mode;
}

void poll_button()
{
    // M5.update() is called by demo_loop on each iteration; we just look at
    // the latched press state.
    if (M5.BtnA.wasPressed()) {
        g_press_start_ms = now_ms_local();
        g_long_press_fired = false;
        g_ap_press_fired   = false;
    }
    const std::uint32_t held = now_ms_local() - g_press_start_ms;
    // 1.5 s while overlay is up → cycle operation_mode + reboot (does not
    // return). Suppressed when AP screen is showing because the user is
    // mid-provisioning; we don't want a wrist-bump to wipe their progress.
    if (M5.BtnA.isPressed() && !g_long_press_fired &&
        held >= kLongPressMs) {
        g_long_press_fired = true;
        if (g_active.load(std::memory_order_relaxed) &&
            !ap_screen::active()) {
            cycle_operation_mode_and_reboot();
            // unreached
        }
    }
    // 5 s while overlay is DOWN → toggle SoftAP provisioning. The threshold
    // is intentionally well past the mode-cycle one so a too-long press on
    // the overlay does NOT spill into AP mode. When the overlay is up the
    // 1.5 s gate already fired the mode cycle (which reboots), so this
    // branch is reachable only from the idle / AP-screen states.
    if (M5.BtnA.isPressed() && !g_ap_press_fired &&
        held >= kApPressMs) {
        g_ap_press_fired = true;
        const bool overlay_up = g_active.load(std::memory_order_relaxed);
        if (!overlay_up || ap_screen::active()) {
            if (wifi_ap_active()) {
                wifi_disable_ap_mode();
            } else {
                wifi_enable_ap_mode();
            }
        }
    }
    if (M5.BtnA.wasReleased()) {
        // Short tap → overlay toggle. Suppress if either long-press path
        // already fired (mode cycle reboots; AP toggle should not also
        // flip the overlay state).
        const bool overlay_up = g_active.load(std::memory_order_relaxed);
        if (!g_long_press_fired && !g_ap_press_fired) {
            g_active.store(!overlay_up, std::memory_order_relaxed);
        } else if (g_long_press_fired && !g_ap_press_fired &&
                   !overlay_up && !ap_screen::active()) {
            // 1.5 s ≤ hold < 5 s from the idle avatar screen → one-touch
            // speaker mute toggle. When the overlay is up the 1.5 s edge
            // already cycled the mode (and rebooted), so reaching here
            // with the flag set means the hold started overlay-down.
            // demo_loop watches the atom and re-applies the M5.Speaker
            // volume; render_task shows the mute badge for feedback.
            if (g_state != nullptr) {
                g_state->speaker.muted.store(
                    !g_state->speaker.muted.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }
        }
        g_long_press_fired = false;
        g_ap_press_fired   = false;
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
    const ConvStatus cs = g_state ? g_state->conv.status.load(std::memory_order_relaxed)
                                  : ConvStatus::Disabled;

    // Format wall-clock time. SNTP sets system time on first WiFi GOT_IP
    // (wifi_sta.cpp); show "--:--:--" until that lands. tm_year > 100 = >2000
    // is a quick way to gate "actually synced" vs "1970 epoch boot value".
    char time_buf[12] = "--:--:--";
    {
        time_t now = 0;
        ::time(&now);
        struct tm tm_now{};
        localtime_r(&now, &tm_now);
        if (tm_now.tm_year > 100) {
            std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
                          tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        }
    }

    // Per-display layout. Small panels (AtomS3 / AtomS3R, 128×128) keep
    // the original compact 12-pt rendering; large round panels (StopWatch,
    // 466×466 AMOLED) get a 24-pt layout centered in the inscribed square
    // so text doesn't fall off the visible circle.
    const bool large = w >= 256;
    const int row_h = large ? 30 : 14;
    const int safe_inset = large
        ? static_cast<int>(w * (1.0f - 0.70710678f) * 0.5f) + 4
        : 4;
    const int key_x = safe_inset;
    const int val_x = safe_inset + (large ? 96 : 40);
    const int y0 = large
        ? (h - 8 * row_h) / 2  // 8 rows centered vertically
        : 6;

    canvas.setFont(large ? &fonts::lgfxJapanGothic_20 : &fonts::lgfxJapanGothic_12);
    canvas.setTextDatum(lgfx::textdatum_t::top_left);

    auto kv = [&](int row, const char* key, const char* value, std::uint16_t vcolor) {
        const int y = y0 + row * row_h;
        canvas.setTextColor(dim);
        canvas.drawString(key, key_x, y);
        canvas.setTextColor(vcolor);
        canvas.drawString(value, val_x, y);
    };

    // Mute marker folded into the Mode row — a dedicated row would push the
    // 128 px layout past the panel edge.
    const bool muted = g_state != nullptr &&
                       g_state->speaker.muted.load(std::memory_order_relaxed);
    char mode_buf[20];
    std::snprintf(mode_buf, sizeof(mode_buf), "%s%s", op_mode_short(g_op_mode),
                  muted ? " (mute)" : "");

    int row = 0;
    kv(row++, "FW",   app ? app->version : "?", fg);
    kv(row++, "SSID", g_ssid.empty() ? "(unset)" : g_ssid.c_str(), fg);
    kv(row++, "IP",   ip.c_str(), wifi ? fg : off);
    kv(row++, "Wifi", wifi ? "up" : "down", wifi ? ok : off);
    kv(row++, "BLE",  ble ? "conn" : "wait", ble ? ok : fg);
    kv(row++, "Conv", conv_status_label(cs), fg);
    kv(row++, "Mode", mode_buf, muted ? off : fg);
    kv(row++, "Time", time_buf, fg);

    // Hint: short tap dismisses; long press cycles mode + reboots.
    canvas.setTextColor(dim);
    canvas.setTextDatum(lgfx::textdatum_t::bottom_center);
    const int hint_y = large ? h - safe_inset : h - 2;
    canvas.drawString("hold BtnA: mode", w / 2, hint_y);
    return true;
}

} // namespace stackchan::app::atom_status
