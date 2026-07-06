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

// Page enum order DOES define the swipe-next / swipe-prev order and the
// tab-bar pagination grouping (kTabsPerPage slots per page), so keep
// closely-related pages adjacent. Settings was split into two pages because
// the 6 toggles + apply row no longer fit under kH=240 with kSettingsRowH=30
// (was overflowing by ~16 px at the bottom).
enum Page : int {
    kInfo = 0,
    kSettings = 1,
    kSettings2 = 2,
    kControl = 3,
    kRange = 4,
    kConversation = 5,
    kLtTimer = 6,
    kTabCount
};
const char* const kTabLabels[kTabCount] = {"情報", "設定1", "設定2", "操作", "範囲", "会話", "LT"};
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
// g_stage_conv mirrors the legacy openai_enabled flag for backwards
// compat with the rest of the file's plumbing; operation_mode is the
// real switch on the UI now.
std::atomic<bool> g_stage_conv{true};
std::atomic<bool> g_stage_rtp{true};
std::atomic<bool> g_stage_bat_gauge{true};
std::atomic<bool> g_stage_boot_arp{true};
// Master servo enable (NVS-persisted). Distinct from g_state->servo.enabled,
// which is the runtime torque toggle on the kControl page.
std::atomic<bool> g_stage_servo_master{true};
// Primary operation mode (config::OperationMode as u8). Cycled by tapping
// the mode row on the settings page.
std::atomic<std::uint8_t> g_stage_op_mode{
    static_cast<std::uint8_t>(stackchan::config::OperationMode::Conversation)};
// Forced output codec selection (config::AudioOutput as u8). Cycled by
// tapping the row on the settings page. Default Auto = honour the
// boot-time probe.
std::atomic<std::uint8_t> g_stage_audio_output{
    static_cast<std::uint8_t>(stackchan::config::AudioOutput::Auto)};

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

// Display origin for the 320×240 UI when the actual panel is larger
// (e.g. StopWatch 466×466 round AMOLED). draw() initialises these from
// the canvas dimensions; handle_tap applies the same translation to the
// incoming touch coords. 0/0 on CoreS3 since its panel is exactly 320×240.
std::int32_t g_off_x = 0;
std::int32_t g_off_y = 0;

// Adapter that forwards every RichCanvas call to an inner canvas, adding
// (ox, oy) to every coordinate. Used so the device_ui can keep drawing as
// if the surface were 320×240 even when it lives in the middle of a
// 466×466 panel. The (vw, vh) reported by width()/height() is the
// virtual 320×240 so internal layout maths (kW / kH) stay correct.
// fillScreen is forwarded as-is — it clears the whole physical surface,
// which is what we want for the avatar background that surrounds the UI.
class OffsetRichCanvas final : public avatar::RichCanvas {
public:
    OffsetRichCanvas(avatar::RichCanvas& inner, std::int32_t ox, std::int32_t oy,
                     std::int32_t vw, std::int32_t vh)
        : inner_{inner}, ox_{ox}, oy_{oy}, vw_{vw}, vh_{vh} {}

    std::int32_t width() const override { return vw_; }
    std::int32_t height() const override { return vh_; }
    void fillScreen(std::uint16_t c) override { inner_.fillScreen(c); }
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::uint16_t c) override { inner_.fillRect(x + ox_, y + oy_, w, h, c); }
    void fillCircle(std::int32_t x, std::int32_t y, std::int32_t r, std::uint16_t c) override {
        inner_.fillCircle(x + ox_, y + oy_, r, c);
    }
    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::int32_t x2, std::int32_t y2, std::uint16_t c) override {
        inner_.fillTriangle(x0 + ox_, y0 + oy_, x1 + ox_, y1 + oy_, x2 + ox_, y2 + oy_, c);
    }
    void begin_group(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override {
        inner_.begin_group(x + ox_, y + oy_, w, h);
    }
    void end_group() override { inner_.end_group(); }
    void begin_frame(std::uint16_t bg) override { inner_.begin_frame(bg); }
    void end_frame() override { inner_.end_frame(); }
    void request_full_repaint() override { inner_.request_full_repaint(); }

    std::uint16_t color565(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return inner_.color565(r, g, b);
    }
    void fillRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint16_t c) override {
        inner_.fillRoundRect(x + ox_, y + oy_, w, h, r, c);
    }
    void drawRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint16_t c) override {
        inner_.drawRoundRect(x + ox_, y + oy_, w, h, r, c);
    }
    void setTextColor(std::uint16_t fg) override { inner_.setTextColor(fg); }
    void setTextColor(std::uint16_t fg, std::uint16_t bg) override { inner_.setTextColor(fg, bg); }
    void setFont(const lgfx::IFont* f) override { inner_.setFont(f); }
    void setTextSize(float s) override { inner_.setTextSize(s); }
    void setTextDatum(lgfx::textdatum_t d) override { inner_.setTextDatum(d); }
    void drawString(const char* s, std::int32_t x, std::int32_t y) override {
        inner_.drawString(s, x + ox_, y + oy_);
    }
    std::int32_t textWidth(const char* s) override { return inner_.textWidth(s); }
    void setClipRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override {
        inner_.setClipRect(x + ox_, y + oy_, w, h);
    }
    void clearClipRect() override { inner_.clearClipRect(); }

private:
    avatar::RichCanvas& inner_;
    std::int32_t ox_, oy_, vw_, vh_;
};

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

// Tighter row pitch for pages with 5+ rows.
// 設定 page has 6 rows (op_mode / audio_output / RTP / battery / servo /
// apply) so 6 * 30 = 180 still fits under 240 - kContentY = 194 with room
// for the footer hint.
constexpr int kSettingsRowH = 30;

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
    const int bat_mv = g_state->battery.mv.load(std::memory_order_relaxed);
    const int bat_ma = g_state->battery.ma.load(std::memory_order_relaxed);
    const int bat_pct = g_state->battery.pct.load(std::memory_order_relaxed);
    char battery[32];
    std::uint16_t bat_color = dim;
    if (bat_pct < 0 || bat_mv < 0) {
        std::snprintf(battery, sizeof(battery), "—");
    } else {
        std::snprintf(battery, sizeof(battery), "%d%% %.2fV %dmA", bat_pct, bat_mv / 1000.0f, bat_ma);
        bat_color = bat_pct >= 50 ? ok : (bat_pct >= 20 ? warn : off);
    }

    int y = kContentY;
    // 10 rows. dy=20 was clipping the last row's text descender by ~6 px;
    // 18 brings the bottom row's baseline to y=46+9*18=208, with text
    // extending to ~y=224 — comfortable headroom under kH=240. Caller can
    // also now swipe to navigate so visual continuity matters more than
    // squeezing every pixel.
    const int dy = 18;
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

// Same shape as draw_toggle_row but the right side shows a textual value
// (e.g. the operation mode label) instead of a pill switch. Tap to cycle.
void draw_value_row(int i, const char* label, const char* value, int row_h = kRowH)
{
    int rx, ry, rw, rh;
    row_rect(i, rx, ry, rw, rh, row_h);
    g_cv->fillRoundRect(rx, ry, rw, rh, 6, g_cv->color565(40, 44, 54));
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_left);
    g_cv->setTextColor(g_cv->color565(235, 235, 235));
    g_cv->drawString(label, rx + 12, ry + rh / 2);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_right);
    g_cv->setTextColor(g_cv->color565(120, 180, 230));
    g_cv->drawString(value, rx + rw - 12, ry + rh / 2);
}

const char* op_mode_label(std::uint8_t m)
{
    switch (m) {
    case 0: return "マイクリップシンク";
    case 1: return "JTTSランダム発話";
    case 2: return "会話応答";
    }
    return "?";
}

const char* audio_output_label(std::uint8_t m)
{
    switch (m) {
    case 0: return "自動";
    case 1: return "内蔵スピーカ";
    case 2: return "Module Audio";
    }
    return "?";
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

// Settings is split across two tabs so the 6 toggles + apply button fit
// under kH=240 with kSettingsRowH=30 each. Apply (= save+reboot) lives only
// on 設定2 — staged values from both pages are merged in apply_and_reboot.
//
//   設定1 — 動作モード / 音声出力 / RTP / 電池ゲージ            (5 rows w/ caption)
//   設定2 — サーボ恒久 / 起動アルペジオ / 適用                   (4 rows w/ caption)
void draw_settings()
{
    draw_value_row(0, "動作モード",
                   op_mode_label(g_stage_op_mode.load(std::memory_order_relaxed)),
                   kSettingsRowH);
    draw_value_row(1, "音声出力",
                   audio_output_label(g_stage_audio_output.load(std::memory_order_relaxed)),
                   kSettingsRowH);
    draw_toggle_row(2, "RTP 音声受信", g_stage_rtp.load(std::memory_order_relaxed), kSettingsRowH);
    draw_toggle_row(3, "電池ゲージ", g_stage_bat_gauge.load(std::memory_order_relaxed), kSettingsRowH);
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::top_left);
    g_cv->setTextColor(g_cv->color565(150, 150, 150));
    g_cv->drawString("→ スワイプで 設定2 / 操作 へ",
                     12, kContentY + 4 * kSettingsRowH + 4);
}

void draw_settings2()
{
    draw_toggle_row(0, "サーボ (恒久)", g_stage_servo_master.load(std::memory_order_relaxed),
                    kSettingsRowH);
    draw_toggle_row(1, "起動アルペジオ", g_stage_boot_arp.load(std::memory_order_relaxed),
                    kSettingsRowH);
    draw_button(2, "適用（保存して再起動）", g_cv->color565(60, 120, 200), kSettingsRowH);
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::top_left);
    g_cv->setTextColor(g_cv->color565(150, 150, 150));
    g_cv->drawString("設定1+2 の変更を「適用」で保存・再起動",
                     12, kContentY + 3 * kSettingsRowH + 4);
}

void draw_control()
{
    const bool servo_on = g_state->servo.enabled.load(std::memory_order_relaxed);
    draw_toggle_row(0, "サーボ（脱力/復帰）", servo_on);
    draw_button(1, "姿勢をリセット", g_cv->color565(60, 120, 200));

    // Speaker volume row: 3 hit zones — left third = -10 %, right third =
    // +10 %, center shows the current value and toggles mute. Live (no
    // reboot) — applied by demo_loop on the next iteration.
    const std::uint16_t pct = g_state->speaker.volume_pct.load(std::memory_order_relaxed);
    int rx, ry, rw, rh;
    row_rect(2, rx, ry, rw, rh);
    g_cv->fillRoundRect(rx, ry, rw, rh, 6, g_cv->color565(40, 44, 54));
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_left);
    g_cv->setTextColor(g_cv->color565(245, 245, 245));
    g_cv->drawString("音量", rx + 12, ry + rh / 2);
    // − / + visual hints on the row edges so the user knows they can tap
    // there to nudge the value.
    g_cv->setTextDatum(lgfx::textdatum_t::middle_center);
    // ASCII '-' / '+' so the default lgfx font renders both correctly
    // (the Unicode minus U+2212 doesn't have a glyph in this build).
    g_cv->drawString("-", rx + rw / 6, ry + rh / 2);
    g_cv->drawString("+", rx + (5 * rw) / 6, ry + rh / 2);
    // Value in the middle, big enough to read at a glance. While muted the
    // value is replaced by a red ミュート label — tapping the center toggles
    // it back (same zone as the mute tap).
    if (g_state->speaker.muted.load(std::memory_order_relaxed)) {
        g_cv->setTextColor(g_cv->color565(230, 110, 110));
        g_cv->drawString("ミュート中", rx + rw / 2, ry + rh / 2);
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u %%", static_cast<unsigned>(pct));
        g_cv->drawString(buf, rx + rw / 2, ry + rh / 2);
    }

    // Row 3: AP-mode toggle. Surfaced on Control (= live runtime actions)
    // rather than Settings because it doesn't stage / require reboot —
    // wifi_enable_ap_mode brings the SoftAP up immediately so iOS users
    // can join and configure Wi-Fi via http://192.168.4.1/. The label
    // reflects the live state so the same row dismisses it.
    const bool ap_on = wifi_ap_active();
    draw_button(3,
                ap_on ? "AP モード（停止）" : "AP モード（Wi-Fi 再設定）",
                ap_on ? g_cv->color565(200, 90, 60)
                       : g_cv->color565(60, 120, 200));
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
    const std::int16_t yaw_raw = g_state->servo.yaw_raw.load(std::memory_order_relaxed);
    const std::int16_t pitch_raw = g_state->servo.pitch_raw.load(std::memory_order_relaxed);
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

    const ConvStatus st = g_state->conv.status.load(std::memory_order_relaxed);
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
    const std::uint32_t reconnects = g_state->conv.reconnects.load(std::memory_order_relaxed);
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

// --- LT (lightning talk) timekeeper page ----------------------------------
//
// Big countdown + presets + start/stop. The timer logic itself lives in
// main/lt_timer.cpp (ticked by demo_loop); this page only reads/writes the
// SharedState atomics. Presets only apply while idle so a stray tap during
// a talk can't change the deadline.

constexpr std::uint16_t kLtPresetsS[] = {180, 300, 600};
constexpr const char* kLtPresetLabels[] = {"3分", "5分", "10分"};
constexpr int kLtPresetY = 150;
constexpr int kLtPresetH = 36;
constexpr int kLtStartY = 196;
constexpr int kLtStartH = 38;

void draw_lt_timer()
{
    const bool active = g_state->lt.active.load(std::memory_order_relaxed);
    const std::int32_t remaining = g_state->lt.remaining_s.load(std::memory_order_relaxed);
    const std::uint16_t total = g_state->lt.total_s.load(std::memory_order_relaxed);

    const std::uint16_t fg = g_cv->color565(235, 235, 235);
    const std::uint16_t dim = g_cv->color565(150, 150, 150);
    const std::uint16_t ok = g_cv->color565(80, 220, 120);
    const std::uint16_t warn = g_cv->color565(235, 200, 90);
    const std::uint16_t err = g_cv->color565(235, 100, 100);

    // Status line.
    const char* status = !active ? "待機中" : (remaining < 0 ? "時間超過!" : "計測中");
    const std::uint16_t status_color = !active ? dim : (remaining < 0 ? err : ok);
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::top_center);
    g_cv->setTextColor(status_color);
    g_cv->drawString(status, kW / 2, kContentY + 2);

    // Big mm:ss readout. Overtime counts up with a leading minus and goes
    // red; the last minute goes amber.
    const std::int32_t shown = remaining < 0 ? -remaining : remaining;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s%d:%02d", remaining < 0 ? "-" : "",
                  static_cast<int>(shown / 60), static_cast<int>(shown % 60));
    const std::uint16_t time_color =
        !active ? fg : (remaining < 0 ? err : (remaining <= 60 ? warn : fg));
    g_cv->setFont(kFontTitle);
    g_cv->setTextSize(2.5f);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_center);
    g_cv->setTextColor(time_color);
    g_cv->drawString(buf, kW / 2, kContentY + 64);
    g_cv->setTextSize(1.0f);

    // Preset row (greyed out while running).
    const int gap = 6;
    const int bw = (kW - 24 - 2 * gap) / 3;
    g_cv->setFont(kFontBody);
    g_cv->setTextDatum(lgfx::textdatum_t::middle_center);
    for (int i = 0; i < 3; ++i) {
        const int bx = 12 + i * (bw + gap);
        const bool selected = (total == kLtPresetsS[i]);
        std::uint16_t bg = active ? g_cv->color565(40, 44, 54)
                                  : (selected ? g_cv->color565(60, 120, 200)
                                              : g_cv->color565(50, 56, 66));
        g_cv->fillRoundRect(bx, kLtPresetY, bw, kLtPresetH, 6, bg);
        g_cv->setTextColor(active ? dim : fg);
        g_cv->drawString(kLtPresetLabels[i], bx + bw / 2, kLtPresetY + kLtPresetH / 2);
    }

    // Start / stop.
    const std::uint16_t btn_color = active ? g_cv->color565(200, 80, 80)
                                           : g_cv->color565(60, 160, 90);
    g_cv->fillRoundRect(12, kLtStartY, kW - 24, kLtStartH, 6, btn_color);
    g_cv->setTextColor(g_cv->color565(245, 245, 245));
    g_cv->drawString(active ? "ストップ" : "スタート", kW / 2, kLtStartY + kLtStartH / 2);
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
    } else if (page == kSettings2) {
        draw_settings2();
    } else if (page == kControl) {
        draw_control();
    } else if (page == kRange) {
        draw_range();
    } else if (page == kLtTimer) {
        draw_lt_timer();
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
    g_stage_boot_arp.store(cfg.startup_arpeggio_enabled, std::memory_order_relaxed);
    g_stage_servo_master.store(cfg.servo_enabled, std::memory_order_relaxed);
    g_stage_op_mode.store(static_cast<std::uint8_t>(cfg.operation_mode),
                          std::memory_order_relaxed);
    g_stage_audio_output.store(static_cast<std::uint8_t>(cfg.audio_output),
                               std::memory_order_relaxed);
    g_stage_limits = parse_servo_limits(cfg.servo_limits_json);
}

void apply_and_reboot()
{
    config::DeviceConfig cfg = config::load();
    cfg.rtp_audio_enabled = g_stage_rtp.load(std::memory_order_relaxed);
    cfg.battery_gauge_enabled = g_stage_bat_gauge.load(std::memory_order_relaxed);
    cfg.startup_arpeggio_enabled = g_stage_boot_arp.load(std::memory_order_relaxed);
    cfg.servo_enabled = g_stage_servo_master.load(std::memory_order_relaxed);
    // operation_mode is now the primary switch — the legacy openai_enabled /
    // jtts_idle_enabled gates are re-derived at boot from this value, so the
    // device_ui doesn't bother touching them here. The previous "conv toggle"
    // semantics still work for older NVS contents via the migration in
    // config_store::load.
    cfg.operation_mode = static_cast<config::OperationMode>(
        g_stage_op_mode.load(std::memory_order_relaxed));
    cfg.audio_output = static_cast<config::AudioOutput>(
        g_stage_audio_output.load(std::memory_order_relaxed));
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
    g_state->servo.range_mode.store(false, std::memory_order_relaxed);
    config::DeviceConfig cfg = config::load();
    cfg.servo_limits_json = format_servo_limits(g_stage_limits);
    (void)config::store::save(cfg);
    esp_restart();
}

// Called on every tab change so range mode is on iff the user is looking at
// the 範囲 page. The servo task picks up the change on its next iteration.
void update_range_mode_for_page(int page)
{
    g_state->servo.range_mode.store(page == kRange, std::memory_order_relaxed);
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
    g_stage_boot_arp.store(cfg.startup_arpeggio_enabled, std::memory_order_relaxed);
    g_stage_servo_master.store(cfg.servo_enabled, std::memory_order_relaxed);
    g_stage_op_mode.store(static_cast<std::uint8_t>(cfg.operation_mode),
                          std::memory_order_relaxed);
    g_stage_audio_output.store(static_cast<std::uint8_t>(cfg.audio_output),
                               std::memory_order_relaxed);
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

void toggle()
{
    if (g_active.load(std::memory_order_relaxed)) {
        // Closing — mirror the close branch of handle_tap's top-bar close
        // hit: drop range mode so the servo task re-engages torque, then
        // clear active.
        update_range_mode_for_page(-1);
        g_active.store(false, std::memory_order_relaxed);
    } else {
        // Opening — same prep as the corner-tap path: reload NVS-backed
        // staged values, jump to 情報 page, force a full repaint.
        load_staged();
        g_page.store(kInfo, std::memory_order_relaxed);
        g_tab_page.store(0, std::memory_order_relaxed);
        g_active.store(true, std::memory_order_relaxed);
        g_dirty.store(true, std::memory_order_relaxed);
    }
}

bool handle_tap(int x, int y)
{
    // Translate physical touch coords into the 320×240 design space the
    // hit-test layout assumes. On panels where the UI is centered (e.g.
    // StopWatch's 466×466 round AMOLED), the avatar's tap-to-open hot
    // corner is shifted by the offset too. Negative results land outside
    // the 320×240 box and harmlessly fail all in_rect() checks.
    x -= g_off_x;
    y -= g_off_y;
    if (!g_active.load(std::memory_order_relaxed)) {
        // Avatar showing: a tap in the top-right corner opens the UI.
        if (x >= kW - 64 && y < 64) {
            load_staged();
            g_page.store(kInfo, std::memory_order_relaxed);
            g_tab_page.store(0, std::memory_order_relaxed); // 情報 is on page 0
            g_active.store(true, std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
            return true;
        }
        // Top-left corner: one-touch speaker mute toggle (mirrors the
        // open-UI hot zone on the opposite corner). demo_loop watches the
        // atom and re-applies the M5.Speaker volume; render_task draws the
        // mute badge in this corner so the zone stays discoverable while
        // muted.
        if (x < 64 && y < 64) {
            g_state->speaker.muted.store(!g_state->speaker.muted.load(std::memory_order_relaxed),
                                         std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Top bar.
    if (y < kBarH) {
        const TabBar b = layout_tabbar();
        if (x >= b.close_x) { // close
            update_range_mode_for_page(-1); // ensure range mode is off on close
            g_active.store(false, std::memory_order_relaxed);
            return true;
        }
        const int np = num_tab_pages();
        if (b.paging && x >= b.prev_x && x < b.prev_x + kArrowW) {
            g_tab_page.store((g_tab_page.load(std::memory_order_relaxed) + np - 1) % np,
                             std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
            return true;
        }
        if (b.paging && x >= b.next_x && x < b.next_x + kArrowW) {
            g_tab_page.store((g_tab_page.load(std::memory_order_relaxed) + 1) % np,
                             std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
            return true;
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
        return true;
    }

    // Content, per page.
    const int page = g_page.load(std::memory_order_relaxed);
    // kSettings packs 5 rows tighter than kRowH so the apply button + caption
    // fit; keep the default kRowH for kControl and the other pages.
    const int row_h = (page == kSettings || page == kSettings2) ? kSettingsRowH : kRowH;
    auto hit_row = [&](int i) {
        int rx, ry, rw, rh;
        row_rect(i, rx, ry, rw, rh, row_h);
        return in_rect(x, y, rx, ry, rw, rh);
    };
    if (page == kSettings) {
        if (hit_row(0)) {
            // Cycle operation_mode 0 → 1 → 2 → 0 ...
            const std::uint8_t cur = g_stage_op_mode.load(std::memory_order_relaxed);
            const std::uint8_t next = (cur + 1) % 3;
            g_stage_op_mode.store(next, std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(1)) {
            // Cycle audio_output 0 (Auto) → 1 (Internal) → 2 (ModuleAudio) → 0 ...
            const std::uint8_t cur = g_stage_audio_output.load(std::memory_order_relaxed);
            const std::uint8_t next = (cur + 1) % 3;
            g_stage_audio_output.store(next, std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(2)) {
            g_stage_rtp.store(!g_stage_rtp.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(3)) {
            g_stage_bat_gauge.store(!g_stage_bat_gauge.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        }
    } else if (page == kSettings2) {
        if (hit_row(0)) {
            g_stage_servo_master.store(!g_stage_servo_master.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(1)) {
            g_stage_boot_arp.store(!g_stage_boot_arp.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(2)) {
            apply_and_reboot(); // does not return
        }
    } else if (page == kControl) {
        if (hit_row(0)) {
            g_state->servo.enabled.store(!g_state->servo.enabled.load(std::memory_order_relaxed),
                                         std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(1)) {
            g_state->servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
            g_state->servo.target_pitch_deg.store(0.0f, std::memory_order_relaxed);
        } else if (hit_row(2)) {
            // Speaker-volume row: left third = -10 %, right third = +10 %,
            // center = mute toggle. Bumps SharedState; demo_loop watches
            // both atoms and pushes the change through the M5.Speaker
            // volume (the pct also persists to NVS; mute does not).
            int rx, ry, rw, rh;
            row_rect(2, rx, ry, rw, rh);
            std::uint16_t pct = g_state->speaker.volume_pct.load(std::memory_order_relaxed);
            const int local_x = x - rx;
            if (local_x < rw / 3) {
                pct = (pct >= 10) ? (pct - 10) : 0;
                g_state->speaker.volume_pct.store(pct, std::memory_order_relaxed);
                g_dirty.store(true, std::memory_order_relaxed);
            } else if (local_x >= (2 * rw) / 3) {
                pct = (pct <= 190) ? (pct + 10) : 200;
                g_state->speaker.volume_pct.store(pct, std::memory_order_relaxed);
                g_dirty.store(true, std::memory_order_relaxed);
            } else {
                g_state->speaker.muted.store(
                    !g_state->speaker.muted.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                g_dirty.store(true, std::memory_order_relaxed);
            }
        } else if (hit_row(3)) {
            // AP-mode toggle. The enable path also starts mDNS / the http
            // settings server (idempotently) and brings up the captive
            // portal so iOS users joining the AP land on the settings
            // page directly. Disable returns the driver to STA-only.
            if (wifi_ap_active()) {
                wifi_disable_ap_mode();
            } else {
                wifi_enable_ap_mode();
            }
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
        const std::int16_t yr = g_state->servo.yaw_raw.load(std::memory_order_relaxed);
        const std::int16_t pr = g_state->servo.pitch_raw.load(std::memory_order_relaxed);
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
    } else if (page == kLtTimer) {
        const bool active = g_state->lt.active.load(std::memory_order_relaxed);
        const int gap = 6;
        const int bw = (kW - 24 - 2 * gap) / 3;
        // Presets — only while idle (see draw_lt_timer's greying).
        if (!active && y >= kLtPresetY && y < kLtPresetY + kLtPresetH) {
            for (int i = 0; i < 3; ++i) {
                const int bx = 12 + i * (bw + gap);
                if (x >= bx && x < bx + bw) {
                    g_state->lt.total_s.store(kLtPresetsS[i], std::memory_order_relaxed);
                    g_state->lt.remaining_s.store(kLtPresetsS[i], std::memory_order_relaxed);
                    g_dirty.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        } else if (in_rect(x, y, 12, kLtStartY, kW - 24, kLtStartH)) {
            // demo_loop's lt_timer.tick consumes the command within ~50 ms.
            g_state->lt.command.store(active ? 2 : 1, std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        }
    }
    // The UI owns the whole screen while active — even a miss on every hit
    // zone must not fall through to the avatar's barge-in path.
    return true;
}

// Horizontal flick → next/prev tab. Vertical flicks (e.g. user scrolling
// intent we don't yet support) and short / too-vertical flicks are ignored
// so a curved drag-style tap doesn't accidentally switch tabs.
void handle_flick(int dx, int dy)
{
    if (!g_active.load(std::memory_order_relaxed)) return;
    // Range mode is a hand-driven page (user is moving the head manually);
    // a horizontal flick is almost certainly trying to capture a position
    // for the buttons, not switch tabs. Same for LT timer running — we
    // don't want a swipe to take the user away mid-talk.
    const int page = g_page.load(std::memory_order_relaxed);
    if (page == kRange) return;
    if (page == kLtTimer && g_state->lt.active.load(std::memory_order_relaxed)) return;

    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    if (adx < 60) return;        // too short to be a deliberate horizontal swipe
    if (adx < 2 * ady) return;   // too vertical — leave for future scroll support

    const int step = (dx < 0) ? +1 : -1; // swipe left = go forward
    int next = page + step;
    if (next < 0)            next = kTabCount - 1;
    if (next >= kTabCount)   next = 0;

    // Mirror the per-page side effects the tab-bar tap path applies:
    // range mode is on iff we're sitting on the range page.
    update_range_mode_for_page(next);
    g_page.store(next, std::memory_order_relaxed);
    // Keep the tab bar's currently-shown slot group aligned with the new
    // page so the active tab is visible in the bar.
    g_tab_page.store(next / kTabsPerPage, std::memory_order_relaxed);
    g_dirty.store(true, std::memory_order_relaxed);
}

bool draw(avatar::RichCanvas& canvas)
{
    // Display offset: center the 320×240 UI on panels larger than that.
    // On CoreS3 (exactly 320×240) offsets are 0; on StopWatch (466×466
    // round AMOLED) they're (73, 113), placing every corner of the 320×240
    // rectangle within radius 200 — well inside the visible 233-radius
    // circle. handle_tap reads these to translate touch coords back.
    g_off_x = (canvas.width() - kW) / 2;
    g_off_y = (canvas.height() - kH) / 2;
    if (g_off_x < 0) g_off_x = 0;
    if (g_off_y < 0) g_off_y = 0;
    OffsetRichCanvas wrapped(canvas, g_off_x, g_off_y, kW, kH);
    g_cv = (g_off_x == 0 && g_off_y == 0) ? &canvas
                                          : static_cast<avatar::RichCanvas*>(&wrapped);

    bool need = g_dirty.exchange(false, std::memory_order_relaxed);
    // The info and 会話 pages have live fields (IP/uptime/heap, conn status /
    // reconnect count) — refresh ~2 Hz so they stay current. 範囲 also has live
    // fields (present-position from the servo task) and wants a faster refresh
    // so the captured raw doesn't lag the user's hand.
    const int page = g_page.load(std::memory_order_relaxed);
    if (page == kInfo || page == kConversation || page == kLtTimer) {
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
