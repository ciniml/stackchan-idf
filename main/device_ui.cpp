// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "device_ui.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <config_service/config_service.hpp>
#include <config_service/config_store.hpp>

#include "servo_limits.hpp"
#include "wifi_sta.hpp"

namespace stackchan::app::ui {

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

// Top bar: tabs + a close button on the right. When there are more tabs than
// fit (kTabsPerPage), the bar paginates with ‹ › arrows.
constexpr int kBarH = 38;
constexpr int kCloseW = 38;
constexpr int kArrowW = 26;
constexpr int kTabsPerPage = 3;

// Content rows.
constexpr int kContentY = kBarH + 8;     // 46
constexpr int kRowH = 42;

enum Page : int { kInfo = 0, kSettings = 1, kControl = 2, kRange = 3, kConversation = 4, kTabCount };
const char* const kTabLabels[kTabCount] = {"情報", "設定", "操作", "範囲", "会話"};
int num_tab_pages() { return (kTabCount + kTabsPerPage - 1) / kTabsPerPage; }

const auto* const kFontTitle = &fonts::lgfxJapanGothic_24;
const auto* const kFontBody = &fonts::lgfxJapanGothic_16;

SharedState* g_state = nullptr;

std::atomic<bool> g_active{false};
std::atomic<int> g_page{kInfo};
std::atomic<int> g_tab_page{0}; // which group of tabs is visible (paging)
std::atomic<bool> g_dirty{true};
int g_provider = 0; // 0 = OpenAI, 1 = Gemini, 2 = XiaoZhi (cached at init)

// Staged settings (loaded from NVS on open; applied on 適用).
std::atomic<bool> g_stage_conv{true};
std::atomic<bool> g_stage_rtp{true};
std::atomic<bool> g_stage_bat_gauge{true};
// Master servo enable (NVS-persisted). Distinct from g_state->servo_enabled,
// which is the runtime torque toggle on the kControl page.
std::atomic<bool> g_stage_servo_master{true};

// Range-setting page: staged ServoLimits (loaded from NVS on tab open, mutated
// by capture taps, saved + reboot on 保存). Plain (non-atomic): the device UI
// runs entirely on the render task, so reads/writes never race.
ServoLimits g_stage_limits;

// Cached once at init() (don't change at runtime) — read by the render task.
std::string g_ssid;
// Snapshot of whether the MCP API has a token set. Cached at init so the
// Info page doesn't have to hit NVS each redraw.
bool g_has_mcp_token = false;
std::string g_host;

// Borrowed each frame from the render task (main owns the drawing strategy).
// The draw helpers below render through this; it is only valid for the duration
// of a draw() call and is never owned/presented here.
avatar::RichCanvas* g_cv = nullptr;
std::uint32_t g_last_info_ms = 0;

std::uint32_t now_ms()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Row hit/draw rectangle (full-width, indented). `row_h` lets callers pack rows
// tighter than the default kRowH=42 when the page has more rows than that fits.
void row_rect(int i, int& rx, int& ry, int& rw, int& rh, int row_h = kRowH)
{
    rx = 10;
    ry = kContentY + i * row_h;
    rw = kW - 20;
    rh = row_h - 6;
}

// Tighter row pitch for pages with 5+ rows. (5 * 36 = 180 < 240 - kContentY = 194.)
constexpr int kSettingsRowH = 36;

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

// --- Tab bar layout (shared by draw + hit-test) --------------------------

struct TabSlot {
    int index; // tab index (Page)
    int x;
    int w;
};
struct TabBar {
    int close_x;
    bool paging = false;
    int prev_x = 0, next_x = 0; // arrow x (width kArrowW) when paging
    TabSlot slots[kTabsPerPage];
    int slot_count = 0;
};

TabBar layout_tabbar()
{
    TabBar b;
    b.close_x = kW - kCloseW;
    if (kTabCount <= kTabsPerPage) {
        const int avail = kW - kCloseW;
        const int tw = avail / kTabCount;
        for (int i = 0; i < kTabCount; ++i) {
            const int x = i * tw;
            const int w = (i == kTabCount - 1) ? (avail - tw * (kTabCount - 1)) : tw;
            b.slots[b.slot_count++] = {i, x, w};
        }
    } else {
        b.paging = true;
        b.prev_x = 0;
        b.next_x = kW - kCloseW - kArrowW;
        const int sx = kArrowW;
        const int sw = b.next_x - sx;
        const int tw = sw / kTabsPerPage;
        const int np = num_tab_pages();
        const int start = (g_tab_page.load(std::memory_order_relaxed) % np) * kTabsPerPage;
        for (int s = 0; s < kTabsPerPage && (start + s) < kTabCount; ++s) {
            b.slots[b.slot_count++] = {start + s, sx + s * tw, tw};
        }
    }
    return b;
}

// --- Drawing -------------------------------------------------------------

void draw_topbar(int page)
{
    const std::uint16_t bar = g_cv->color565(40, 44, 54);
    const std::uint16_t sel = g_cv->color565(60, 120, 200);
    const std::uint16_t fg = g_cv->color565(235, 235, 235);
    const std::uint16_t arrow = g_cv->color565(70, 74, 84);
    g_cv->fillRect(0, 0, kW, kBarH, bar);

    const TabBar b = layout_tabbar();
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_center);

    if (b.paging) {
        g_cv->fillRect(b.prev_x, 0, kArrowW, kBarH, arrow);
        g_cv->fillRect(b.next_x, 0, kArrowW, kBarH, arrow);
        // Draw the paging arrows as triangles (font has no ‹ › glyphs).
        const int cy = kBarH / 2;
        const int pcx = b.prev_x + kArrowW / 2;
        g_cv->fillTriangle(pcx - 5, cy, pcx + 4, cy - 7, pcx + 4, cy + 7, fg); // ◀
        const int ncx = b.next_x + kArrowW / 2;
        g_cv->fillTriangle(ncx + 5, cy, ncx - 4, cy - 7, ncx - 4, cy + 7, fg); // ▶
    }
    for (int i = 0; i < b.slot_count; ++i) {
        const TabSlot& s = b.slots[i];
        if (s.index == page) g_cv->fillRect(s.x, 0, s.w, kBarH, sel);
        g_cv->setTextColor(fg);
        g_cv->drawString(kTabLabels[s.index], s.x + s.w / 2, kBarH / 2);
    }
    // Close button.
    g_cv->fillRect(b.close_x, 0, kCloseW, kBarH, g_cv->color565(120, 60, 60));
    g_cv->setTextColor(fg);
    g_cv->drawString("×", b.close_x + kCloseW / 2, kBarH / 2);
}

void draw_kv(int y, const char* key, const char* value, std::uint16_t vcolor)
{
    const std::uint16_t dim = g_cv->color565(150, 150, 150);
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::top_left);
    g_cv->setTextColor(dim);
    g_cv->drawString(key, 12, y);
    g_cv->setTextColor(vcolor);
    g_cv->drawString(value, 120, y);
}

void draw_info()
{
    const std::uint16_t fg = g_cv->color565(235, 235, 235);
    const std::uint16_t ok = g_cv->color565(80, 220, 120);
    const std::uint16_t off = g_cv->color565(230, 110, 110);
    const std::uint16_t warn = g_cv->color565(235, 200, 90);
    const std::uint16_t dim = g_cv->color565(150, 150, 150);

    const esp_app_desc_t* app = esp_app_get_description();
    const bool wifi = wifi_is_connected();
    const bool ble = config::ble_connected();
    const std::string ip = current_ip();
    const std::uint32_t up = now_ms() / 1000;
    char uptime[24];
    if (up >= 3600) {
        std::snprintf(uptime, sizeof(uptime), "%uh%02um%02us", static_cast<unsigned>(up / 3600),
                      static_cast<unsigned>((up % 3600) / 60), static_cast<unsigned>(up % 60));
    } else {
        std::snprintf(uptime, sizeof(uptime), "%um%02us", static_cast<unsigned>(up / 60),
                      static_cast<unsigned>(up % 60));
    }
    char heap[16];
    std::snprintf(heap, sizeof(heap), "%u KB", static_cast<unsigned>(esp_get_free_heap_size() / 1024));

    // Battery (INA226, refreshed by demo_loop). pct < 0 → not yet read / absent.
    const int bat_mv = g_state->battery_mv.load(std::memory_order_relaxed);
    const int bat_ma = g_state->battery_ma.load(std::memory_order_relaxed);
    const int bat_pct = g_state->battery_pct.load(std::memory_order_relaxed);
    char battery[32];
    std::uint16_t bat_color = dim;
    if (bat_pct < 0 || bat_mv < 0) {
        std::snprintf(battery, sizeof(battery), "—");
    } else {
        std::snprintf(battery, sizeof(battery), "%d%% %.2fV %dmA", bat_pct, bat_mv / 1000.0f, bat_ma);
        bat_color = bat_pct >= 50 ? ok : (bat_pct >= 20 ? warn : off);
    }

    int y = kContentY;
    // 10 rows now (added MCP API state) — tighten dy from 22 → 20 so the
    // bottom row fits under kH=240. (46 + 10*20 = 246; the last row's text
    // baseline sits within the screen with a few px of slack.)
    const int dy = 20;
    draw_kv(y, "FW", app ? app->version : "?", fg); y += dy;
    draw_kv(y, "SSID", g_ssid.empty() ? "(未設定)" : g_ssid.c_str(), fg); y += dy;
    draw_kv(y, "mDNS", g_host.c_str(), fg); y += dy;
    draw_kv(y, "IP", ip.c_str(), wifi ? fg : off); y += dy;
    draw_kv(y, "Wi-Fi", wifi ? "接続中" : "未接続", wifi ? ok : off); y += dy;
    draw_kv(y, "BLE", ble ? "接続中" : "待受中", ble ? ok : fg); y += dy;
    draw_kv(y, "稼働", uptime, fg); y += dy;
    draw_kv(y, "空きRAM", heap, fg); y += dy;
    draw_kv(y, "電池", battery, bat_color); y += dy;
    draw_kv(y, "MCP", g_has_mcp_token ? "設定済" : "未設定",
            g_has_mcp_token ? ok : dim); y += dy;
}

void draw_toggle_row(int i, const char* label, bool on, int row_h = kRowH)
{
    int rx, ry, rw, rh;
    row_rect(i, rx, ry, rw, rh, row_h);
    g_cv->fillRoundRect(rx, ry, rw, rh, 6, g_cv->color565(40, 44, 54));
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_left);
    g_cv->setTextColor(g_cv->color565(235, 235, 235));
    g_cv->drawString(label, rx + 12, ry + rh / 2);
    // Pill switch on the right.
    const int pw = 58, ph = 26;
    const int px = rx + rw - pw - 10, py = ry + (rh - ph) / 2;
    const std::uint16_t on_c = g_cv->color565(80, 200, 120);
    const std::uint16_t off_c = g_cv->color565(90, 90, 96);
    g_cv->fillRoundRect(px, py, pw, ph, ph / 2, on ? on_c : off_c);
    const int kn = ph - 6;
    g_cv->fillCircle(on ? (px + pw - ph / 2) : (px + ph / 2), py + ph / 2, kn / 2,
                        g_cv->color565(245, 245, 245));
}

void draw_button(int i, const char* label, std::uint16_t color, int row_h = kRowH)
{
    int rx, ry, rw, rh;
    row_rect(i, rx, ry, rw, rh, row_h);
    g_cv->fillRoundRect(rx, ry, rw, rh, 6, color);
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_center);
    g_cv->setTextColor(g_cv->color565(245, 245, 245));
    g_cv->drawString(label, rx + rw / 2, ry + rh / 2);
}

void draw_settings()
{
    draw_toggle_row(0, "会話機能", g_stage_conv.load(std::memory_order_relaxed), kSettingsRowH);
    draw_toggle_row(1, "RTP 音声受信", g_stage_rtp.load(std::memory_order_relaxed), kSettingsRowH);
    draw_toggle_row(2, "電池ゲージ", g_stage_bat_gauge.load(std::memory_order_relaxed), kSettingsRowH);
    draw_toggle_row(3, "サーボ (恒久)", g_stage_servo_master.load(std::memory_order_relaxed),
                    kSettingsRowH);
    draw_button(4, "適用（保存して再起動）", g_cv->color565(60, 120, 200), kSettingsRowH);
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::top_left);
    g_cv->setTextColor(g_cv->color565(150, 150, 150));
    g_cv->drawString("変更は適用で反映されます", 12, kContentY + 5 * kSettingsRowH + 2);
}

void draw_control()
{
    const bool servo_on = g_state->servo_enabled.load(std::memory_order_relaxed);
    draw_toggle_row(0, "サーボ（脱力/復帰）", servo_on);
    draw_button(1, "姿勢をリセット", g_cv->color565(60, 120, 200));
}

// --- 範囲設定 (servo range-setting) page --------------------------------------
//
// While this page is the active tab, servo_range_mode is held true (set in
// handle_tap on tab change, cleared on leave). The servo task drops torque so
// the user moves the head by hand; the on-screen Y / P readouts update from
// the periodically-polled present-positions. Buttons capture the live raw
// position as zero / min / max for the displayed axis. 保存 writes the
// resulting limits to NVS and reboots so the servo task picks them up.

// Layout: kRowH is too tall to fit two display rows + two button rows + a
// save row, so the range page uses its own tighter geometry.
constexpr int kRangeInfoH = 30;     // per-axis info row
constexpr int kRangeBtnRowH = 36;   // per-axis 3-button row
constexpr int kRangeSaveH = 40;     // bottom save button

struct RangeLayout {
    int y_yaw_info;
    int y_yaw_btns;
    int y_pitch_info;
    int y_pitch_btns;
    int y_save;
    int btn_w;   // width of each of the three capture buttons
    int save_w;
};
RangeLayout layout_range()
{
    RangeLayout L;
    L.y_yaw_info = kContentY;
    L.y_yaw_btns = L.y_yaw_info + kRangeInfoH;
    L.y_pitch_info = L.y_yaw_btns + kRangeBtnRowH + 2;
    L.y_pitch_btns = L.y_pitch_info + kRangeInfoH;
    L.y_save = L.y_pitch_btns + kRangeBtnRowH + 4;
    L.btn_w = (kW - 24) / 3; // 8 px outer padding, 3 buttons
    L.save_w = kW - 24;
    return L;
}

// Round a (raw - zero) step delta to integer degrees. SCS0009: 1 step ≈ 0.3125°.
int raw_delta_to_deg(int raw, int zero)
{
    const float d = (raw - zero) * 5.0f / 16.0f;
    return static_cast<int>(d >= 0 ? d + 0.5f : d - 0.5f);
}

void draw_range_info(int y, const char* axis, int live_raw, int zero, int lo_deg,
                     int hi_deg)
{
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::top_left);
    g_cv->setTextColor(g_cv->color565(235, 235, 235));
    char line[64];
    if (live_raw < 0) {
        std::snprintf(line, sizeof(line), "%s: --   z=%d  [%d, %d]°", axis, zero,
                      lo_deg, hi_deg);
    } else {
        const int cur_deg = raw_delta_to_deg(live_raw, zero);
        std::snprintf(line, sizeof(line), "%s: %d (%+d°)  z=%d  [%d, %d]°", axis,
                      live_raw, cur_deg, zero, lo_deg, hi_deg);
    }
    g_cv->drawString(line, 12, y + 4);
}

void draw_range_btn(int x, int y, int w, int h, const char* label, std::uint16_t color)
{
    g_cv->fillRoundRect(x, y, w, h, 6, color);
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_center);
    g_cv->setTextColor(g_cv->color565(245, 245, 245));
    g_cv->drawString(label, x + w / 2, y + h / 2);
}

void draw_range()
{
    const RangeLayout L = layout_range();
    const std::int16_t yaw_raw = g_state->servo_yaw_raw.load(std::memory_order_relaxed);
    const std::int16_t pitch_raw = g_state->servo_pitch_raw.load(std::memory_order_relaxed);
    const std::uint16_t zero_c = g_cv->color565(70, 110, 170);
    const std::uint16_t min_c = g_cv->color565(120, 80, 160);
    const std::uint16_t max_c = g_cv->color565(160, 100, 70);

    draw_range_info(L.y_yaw_info, "Y", yaw_raw, g_stage_limits.yaw_zero,
                    g_stage_limits.yaw_min_deg, g_stage_limits.yaw_max_deg);
    const int x0 = 12;
    const int gap = 4;
    const int bw = (kW - 24 - 2 * gap) / 3;
    draw_range_btn(x0,                 L.y_yaw_btns, bw, kRangeBtnRowH - 4, "Y0", zero_c);
    draw_range_btn(x0 + bw + gap,      L.y_yaw_btns, bw, kRangeBtnRowH - 4, "Y-", min_c);
    draw_range_btn(x0 + 2 * (bw + gap), L.y_yaw_btns, bw, kRangeBtnRowH - 4, "Y+", max_c);

    draw_range_info(L.y_pitch_info, "P", pitch_raw, g_stage_limits.pitch_zero,
                    g_stage_limits.pitch_min_deg, g_stage_limits.pitch_max_deg);
    draw_range_btn(x0,                 L.y_pitch_btns, bw, kRangeBtnRowH - 4, "P0", zero_c);
    draw_range_btn(x0 + bw + gap,      L.y_pitch_btns, bw, kRangeBtnRowH - 4, "P-", min_c);
    draw_range_btn(x0 + 2 * (bw + gap), L.y_pitch_btns, bw, kRangeBtnRowH - 4, "P+", max_c);

    draw_range_btn(12, L.y_save, kW - 24, kRangeSaveH, "保存（再起動）",
                   g_cv->color565(60, 140, 80));
}

void draw_conversation()
{
    const std::uint16_t fg = g_cv->color565(235, 235, 235);
    const std::uint16_t ok = g_cv->color565(80, 220, 120);
    const std::uint16_t warn = g_cv->color565(235, 200, 90);
    const std::uint16_t err = g_cv->color565(235, 100, 100);
    const std::uint16_t dim = g_cv->color565(150, 150, 150);

    const ConvStatus st = g_state->conversation_status.load(std::memory_order_relaxed);
    const char* status_text = "?";
    std::uint16_t status_color = dim;
    switch (st) {
    case ConvStatus::Disabled: status_text = "無効"; status_color = dim; break;
    case ConvStatus::WaitingWifi: status_text = "Wi-Fi 待ち"; status_color = warn; break;
    case ConvStatus::Connecting: status_text = "接続中…"; status_color = warn; break;
    case ConvStatus::Listening: status_text = "接続（待受）"; status_color = ok; break;
    case ConvStatus::Talking: status_text = "通話中"; status_color = ok; break;
    case ConvStatus::Yielded: status_text = "音声再生中"; status_color = dim; break;
    case ConvStatus::Reconnecting: status_text = "再接続中…"; status_color = warn; break;
    case ConvStatus::Error: status_text = "接続エラー"; status_color = err; break;
    }
    const std::uint32_t reconnects = g_state->conversation_reconnects.load(std::memory_order_relaxed);
    char rc[16];
    std::snprintf(rc, sizeof(rc), "%u 回", static_cast<unsigned>(reconnects));

    int y = kContentY;
    const int dy = 26;
    const char* provider_name = g_provider == 1   ? "Gemini Live"
                                : g_provider == 2 ? "XiaoZhi"
                                                  : "OpenAI Realtime";
    draw_kv(y, "サービス", provider_name, fg); y += dy;
    draw_kv(y, "状態", status_text, status_color); y += dy;
    draw_kv(y, "再接続", rc, reconnects > 0 ? warn : fg); y += dy;
}

void render_page()
{
    const int page = g_page.load(std::memory_order_relaxed);
    g_cv->fillScreen(g_cv->color565(20, 22, 28));
    draw_topbar(page);
    if (page == kInfo) {
        draw_info();
    } else if (page == kSettings) {
        draw_settings();
    } else if (page == kControl) {
        draw_control();
    } else if (page == kRange) {
        draw_range();
    } else {
        draw_conversation();
    }
    // No pushSprite here — the render task owns the canvas and pushes it.
}

// --- Actions -------------------------------------------------------------

void load_staged()
{
    const config::DeviceConfig cfg = config::load();
    g_stage_conv.store(cfg.openai_enabled, std::memory_order_relaxed);
    g_stage_rtp.store(cfg.rtp_audio_enabled, std::memory_order_relaxed);
    g_stage_bat_gauge.store(cfg.battery_gauge_enabled, std::memory_order_relaxed);
    g_stage_servo_master.store(cfg.servo_enabled, std::memory_order_relaxed);
    g_stage_limits = parse_servo_limits(cfg.servo_limits_json);
}

void apply_and_reboot()
{
    config::DeviceConfig cfg = config::load();
    cfg.openai_enabled = g_stage_conv.load(std::memory_order_relaxed);
    cfg.rtp_audio_enabled = g_stage_rtp.load(std::memory_order_relaxed);
    cfg.battery_gauge_enabled = g_stage_bat_gauge.load(std::memory_order_relaxed);
    cfg.servo_enabled = g_stage_servo_master.load(std::memory_order_relaxed);
    (void)config::store::save(cfg);
    esp_restart();
}

// Serialize the staged ServoLimits as the compact JSON parse_servo_limits expects,
// persist to NVS, and reboot so the servo task picks up the new limits.
std::string format_servo_limits(const ServoLimits& l)
{
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "{\"yaw_zero\":%u,\"yaw_min\":%d,\"yaw_max\":%d,"
                  "\"pitch_zero\":%u,\"pitch_min\":%d,\"pitch_max\":%d}",
                  static_cast<unsigned>(l.yaw_zero), l.yaw_min_deg, l.yaw_max_deg,
                  static_cast<unsigned>(l.pitch_zero), l.pitch_min_deg, l.pitch_max_deg);
    return std::string(buf);
}

void save_range_and_reboot()
{
    // Release range mode so the new limits aren't shadowed by the
    // "torque off / no goal writes" branch on the way down (servo task may
    // briefly re-engage between this call and esp_restart).
    g_state->servo_range_mode.store(false, std::memory_order_relaxed);
    config::DeviceConfig cfg = config::load();
    cfg.servo_limits_json = format_servo_limits(g_stage_limits);
    (void)config::store::save(cfg);
    esp_restart();
}

// Called on every tab change so range mode is on iff the user is looking at
// the 範囲 page. The servo task picks up the change on its next iteration.
void update_range_mode_for_page(int page)
{
    g_state->servo_range_mode.store(page == kRange, std::memory_order_relaxed);
}

} // namespace

void init(SharedState& state)
{
    g_state = &state;
    const config::DeviceConfig cfg = config::load();
    g_ssid = cfg.wifi_ssid;
    g_has_mcp_token = !cfg.mcp_api_token.empty();
    g_provider = static_cast<int>(cfg.provider);
    g_stage_conv.store(cfg.openai_enabled, std::memory_order_relaxed);
    g_stage_rtp.store(cfg.rtp_audio_enabled, std::memory_order_relaxed);
    g_stage_bat_gauge.store(cfg.battery_gauge_enabled, std::memory_order_relaxed);
    g_stage_servo_master.store(cfg.servo_enabled, std::memory_order_relaxed);
    std::uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char host[32];
    std::snprintf(host, sizeof(host), "stackchan-%02x%02x%02x.local", mac[3], mac[4], mac[5]);
    g_host = host;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

void handle_tap(int x, int y)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        // Avatar showing: a tap in the top-right corner opens the UI.
        if (x >= kW - 64 && y < 64) {
            load_staged();
            g_page.store(kInfo, std::memory_order_relaxed);
            g_tab_page.store(0, std::memory_order_relaxed); // 情報 is on page 0
            g_active.store(true, std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        }
        return;
    }

    // Top bar.
    if (y < kBarH) {
        const TabBar b = layout_tabbar();
        if (x >= b.close_x) { // close
            update_range_mode_for_page(-1); // ensure range mode is off on close
            g_active.store(false, std::memory_order_relaxed);
            return;
        }
        const int np = num_tab_pages();
        if (b.paging && x >= b.prev_x && x < b.prev_x + kArrowW) {
            g_tab_page.store((g_tab_page.load(std::memory_order_relaxed) + np - 1) % np,
                             std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
            return;
        }
        if (b.paging && x >= b.next_x && x < b.next_x + kArrowW) {
            g_tab_page.store((g_tab_page.load(std::memory_order_relaxed) + 1) % np,
                             std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
            return;
        }
        for (int i = 0; i < b.slot_count; ++i) {
            if (x >= b.slots[i].x && x < b.slots[i].x + b.slots[i].w) {
                const int new_page = b.slots[i].index;
                if (new_page == kRange) {
                    // Re-load the staged limits each time we enter the page so
                    // editor state matches whatever's currently in NVS.
                    load_staged();
                }
                g_page.store(new_page, std::memory_order_relaxed);
                update_range_mode_for_page(new_page);
                g_dirty.store(true, std::memory_order_relaxed);
                break;
            }
        }
        return;
    }

    // Content, per page.
    const int page = g_page.load(std::memory_order_relaxed);
    // kSettings packs 5 rows tighter than kRowH so the apply button + caption
    // fit; keep the default kRowH for kControl and the other pages.
    const int row_h = (page == kSettings) ? kSettingsRowH : kRowH;
    auto hit_row = [&](int i) {
        int rx, ry, rw, rh;
        row_rect(i, rx, ry, rw, rh, row_h);
        return in_rect(x, y, rx, ry, rw, rh);
    };
    if (page == kSettings) {
        if (hit_row(0)) {
            g_stage_conv.store(!g_stage_conv.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(1)) {
            g_stage_rtp.store(!g_stage_rtp.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(2)) {
            g_stage_bat_gauge.store(!g_stage_bat_gauge.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(3)) {
            g_stage_servo_master.store(!g_stage_servo_master.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(4)) {
            apply_and_reboot(); // does not return
        }
    } else if (page == kControl) {
        if (hit_row(0)) {
            g_state->servo_enabled.store(!g_state->servo_enabled.load(std::memory_order_relaxed),
                                         std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(1)) {
            g_state->target_yaw_deg.store(0.0f, std::memory_order_relaxed);
            g_state->target_pitch_deg.store(0.0f, std::memory_order_relaxed);
        }
    } else if (page == kRange) {
        const RangeLayout L = layout_range();
        const int x0 = 12;
        const int gap = 4;
        const int bw = (kW - 24 - 2 * gap) / 3;
        const int bh = kRangeBtnRowH - 4;
        auto hit_btn = [&](int bx, int by) {
            return in_rect(x, y, bx, by, bw, bh);
        };
        const std::int16_t yr = g_state->servo_yaw_raw.load(std::memory_order_relaxed);
        const std::int16_t pr = g_state->servo_pitch_raw.load(std::memory_order_relaxed);
        bool changed = false;
        if (hit_btn(x0, L.y_yaw_btns) && yr >= 0) {
            g_stage_limits.yaw_zero = static_cast<std::uint16_t>(yr);
            changed = true;
        } else if (hit_btn(x0 + bw + gap, L.y_yaw_btns) && yr >= 0) {
            g_stage_limits.yaw_min_deg = raw_delta_to_deg(yr, g_stage_limits.yaw_zero);
            changed = true;
        } else if (hit_btn(x0 + 2 * (bw + gap), L.y_yaw_btns) && yr >= 0) {
            g_stage_limits.yaw_max_deg = raw_delta_to_deg(yr, g_stage_limits.yaw_zero);
            changed = true;
        } else if (hit_btn(x0, L.y_pitch_btns) && pr >= 0) {
            g_stage_limits.pitch_zero = static_cast<std::uint16_t>(pr);
            changed = true;
        } else if (hit_btn(x0 + bw + gap, L.y_pitch_btns) && pr >= 0) {
            g_stage_limits.pitch_min_deg = raw_delta_to_deg(pr, g_stage_limits.pitch_zero);
            changed = true;
        } else if (hit_btn(x0 + 2 * (bw + gap), L.y_pitch_btns) && pr >= 0) {
            g_stage_limits.pitch_max_deg = raw_delta_to_deg(pr, g_stage_limits.pitch_zero);
            changed = true;
        } else if (in_rect(x, y, 12, L.y_save, kW - 24, kRangeSaveH)) {
            save_range_and_reboot(); // does not return
        }
        if (changed) g_dirty.store(true, std::memory_order_relaxed);
    }
}

bool draw(avatar::RichCanvas& canvas)
{
    g_cv = &canvas; // borrowed for this frame; the draw helpers render through it

    bool need = g_dirty.exchange(false, std::memory_order_relaxed);
    // The info and 会話 pages have live fields (IP/uptime/heap, conn status /
    // reconnect count) — refresh ~2 Hz so they stay current. 範囲 also has live
    // fields (present-position from the servo task) and wants a faster refresh
    // so the captured raw doesn't lag the user's hand.
    const int page = g_page.load(std::memory_order_relaxed);
    if (page == kInfo || page == kConversation) {
        const std::uint32_t t = now_ms();
        if (t - g_last_info_ms > 500) {
            g_last_info_ms = t;
            need = true;
        }
    } else if (page == kRange) {
        const std::uint32_t t = now_ms();
        if (t - g_last_info_ms > 150) {
            g_last_info_ms = t;
            need = true;
        }
    }
    if (need) {
        render_page();
    }
    return need;
}

} // namespace stackchan::app::ui
