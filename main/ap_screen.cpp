// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "ap_screen.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "wifi_sta.hpp"

namespace stackchan::app::ap_screen {

namespace {

// Track the last (SSID, PW, IP) tuple painted so we only redraw when the
// driver flips state. The QR encode + multi-rect fillRect is expensive on
// large round panels (StopWatch 466×466) and we don't want to burn it every
// frame at 30 fps.
char g_last_ssid[33] = {0};
char g_last_pw[33]   = {0};
char g_last_ip[16]   = {0};
bool g_painted = false;

// Exit-button rect in panel coords (set by the most recent draw call).
// handle_tap hit-tests against these. Boards without LCD touch (AtomS3R /
// AtomS3 — the "tiny" branch) leave btn_w = 0; handle_tap then never matches
// and still consumes the tap to keep the touchless gesture path clean.
int g_btn_x = 0;
int g_btn_y = 0;
int g_btn_w = 0;
int g_btn_h = 0;

// Build the standard Wi-Fi QR payload. Format documented by the Wi-Fi
// Alliance and supported natively by iOS Camera / Android scanners:
//   WIFI:T:<auth>;S:<SSID>;P:<password>;H:<hidden>;;
// Special chars `\`, `;`, `,`, `"`, `:` must be escaped with `\` inside SSID
// and password — our derived SSID/PW use only [A-Za-z0-9-], so no escaping
// is actually needed; we still apply it for safety should the credential
// derivation ever change.
std::size_t append_escaped(char* dst, std::size_t cap, std::size_t pos, const char* src)
{
    for (; *src != '\0' && pos + 2 < cap; ++src) {
        const char c = *src;
        if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':') {
            dst[pos++] = '\\';
        }
        dst[pos++] = c;
    }
    dst[pos] = '\0';
    return pos;
}

void build_wifi_qr_payload(const char* ssid, const char* pw, char* out, std::size_t cap)
{
    std::size_t p = 0;
    constexpr const char* kPrefix = "WIFI:T:WPA;S:";
    constexpr const char* kMid    = ";P:";
    constexpr const char* kSuffix = ";H:false;;";
    p += std::snprintf(out + p, cap - p, "%s", kPrefix);
    p = append_escaped(out, cap, p, ssid);
    p += std::snprintf(out + p, cap - p, "%s", kMid);
    p = append_escaped(out, cap, p, pw);
    std::snprintf(out + p, cap - p, "%s", kSuffix);
}

} // namespace

bool active()
{
    return wifi_ap_active();
}

bool draw(M5GFX& display)
{
    char ssid[33], pw[33], ip[16];
    if (!wifi_ap_info(ssid, sizeof(ssid), pw, sizeof(pw), ip, sizeof(ip))) {
        return false;
    }
    // Only repaint when the credentials actually changed — saves the full
    // QR render cost on every render-task tick.
    if (g_painted &&
        std::strcmp(ssid, g_last_ssid) == 0 &&
        std::strcmp(pw, g_last_pw) == 0 &&
        std::strcmp(ip, g_last_ip) == 0) {
        return false;
    }
    std::strncpy(g_last_ssid, ssid, sizeof(g_last_ssid) - 1);
    std::strncpy(g_last_pw, pw, sizeof(g_last_pw) - 1);
    std::strncpy(g_last_ip, ip, sizeof(g_last_ip) - 1);

    const std::int32_t W = display.width();
    const std::int32_t H = display.height();

    // Layout breakpoints:
    //   - tiny (≤160 px): 128×128 atom panels — no touch, so no exit button;
    //                     atom_status's 5 s long-press gesture dismisses AP.
    //   - mid:           CoreS3 320×240 — QR centred with title above, three
    //                    text lines below, exit-button band at the bottom.
    //   - round (≥400):  StopWatch 466×466 AMOLED — content kept inside the
    //                    inscribed square (~0.707 R) horizontally but allowed
    //                    closer to the panel edge along the vertical centre
    //                    axis (where the visible circle reaches almost full
    //                    height).
    const bool tiny  = W <= 160;
    const bool round_ = W >= 400;

    const int inset = round_
        ? static_cast<int>(W * (1.0f - 0.70710678f) * 0.5f) + 4
        : 4;

    // Bottom exit-button band. Height ~ 1/6 of panel height on touch boards.
    // On round panels the band can extend slightly below the inscribed
    // square because the button is centred horizontally — the visible
    // circle is widest along the vertical axis, narrowest at the corners.
    const int btn_h = tiny ? 0
                    : round_ ? 40
                    : 30;
    const int btn_band_y = tiny ? H : (H - btn_h - (round_ ? 18 : 4));
    const int btn_w = tiny ? 0
                    : round_ ? 200
                    : (W - 24);
    const int btn_x = (W - btn_w) / 2;

    // QR + caption block fills the area above the button band. Pick a QR
    // size that leaves room for the title (top) and 3 caption lines plus
    // the button band (bottom).
    const int caption_lines = tiny ? 1 : 3;
    const int caption_line_h = round_ ? 22 : 14;
    const int caption_h = caption_lines * caption_line_h + 4;
    const int title_h = tiny ? 0 : (round_ ? 32 : 20);
    const int top_margin = tiny ? 8 : (round_ ? inset : 4);

    const int avail_y_for_qr = btn_band_y - top_margin - title_h - caption_h;
    const int qr_max_by_w = tiny ? (W - 18)
                          : round_ ? (W * 220 / 466)
                          : 150;
    const int qr_w = (avail_y_for_qr < qr_max_by_w) ? avail_y_for_qr : qr_max_by_w;
    const int qr_x = (W - qr_w) / 2;
    const int qr_y = top_margin + title_h;

    g_btn_x = btn_x;
    g_btn_y = btn_band_y;
    g_btn_w = btn_w;
    g_btn_h = btn_h;

    char qr_payload[160];
    build_wifi_qr_payload(ssid, pw, qr_payload, sizeof(qr_payload));

    display.startWrite();
    display.fillScreen(TFT_WHITE);

    // Title — skipped on the tiny panel (no space).
    if (!tiny) {
        display.setFont(round_ ? &fonts::lgfxJapanGothic_24
                                : &fonts::lgfxJapanGothic_16);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        display.drawString("Wi-Fi 設定モード", W / 2, top_margin);
    }

    // QR. Version 0 = auto-pick by payload length. The payload is ≤ 80
    // chars in practice, well within an auto-sized QR at the default ECC.
    display.qrcode(qr_payload, qr_x, qr_y, qr_w, 0);

    // Caption block between the QR and the exit button.
    if (!tiny) {
        display.setFont(round_ ? &fonts::lgfxJapanGothic_20
                                : &fonts::lgfxJapanGothic_12);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        int ty = qr_y + qr_w + 4;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSID: %s", ssid);
        display.drawString(buf, W / 2, ty);
        ty += caption_line_h;
        std::snprintf(buf, sizeof(buf), "PW:   %s", pw);
        display.drawString(buf, W / 2, ty);
        ty += caption_line_h;
        std::snprintf(buf, sizeof(buf), "http://%s/", ip);
        display.drawString(buf, W / 2, ty);
    } else {
        // 128×128: only the SSID fits readably under the QR. iOS-camera-
        // scan-then-tap-join carries the URL via the captive portal probe,
        // so the URL line isn't critical here.
        display.setFont(&fonts::Font0);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        display.drawString(ssid, W / 2, qr_y + qr_w + 2);
    }

    // Exit button (touch boards only). Red so it reads as "stop/cancel"
    // rather than a neutral CTA — same colour treatment as device_ui's
    // "AP モード（停止）" button when AP is up.
    if (btn_w > 0 && btn_h > 0) {
        const std::uint16_t red = display.color565(200, 90, 60);
        display.fillRoundRect(btn_x, btn_band_y, btn_w, btn_h, 6, red);
        display.setFont(round_ ? &fonts::lgfxJapanGothic_20
                                : &fonts::lgfxJapanGothic_16);
        display.setTextColor(TFT_WHITE, red);
        display.setTextDatum(lgfx::textdatum_t::middle_center);
        display.drawString("終了 (AP モード停止)",
                           btn_x + btn_w / 2, btn_band_y + btn_h / 2);
    }
    display.endWrite();

    g_painted = true;
    return true;
}

bool handle_tap(int x, int y)
{
    if (!wifi_ap_active()) return false;
    // While AP is up we own the touch input — swallow every tap even if it
    // missed the exit button. Otherwise the tap would propagate to the
    // hidden device_ui underneath (whose top-right corner opens the 5-tab
    // settings UI), which would surprise the user when AP is later
    // dismissed and the UI reappears already open.
    if (g_btn_w > 0 && g_btn_h > 0 &&
        x >= g_btn_x && x < g_btn_x + g_btn_w &&
        y >= g_btn_y && y < g_btn_y + g_btn_h) {
        wifi_disable_ap_mode();
        // Mark the cached snapshot stale so the next render task tick that
        // sees ap_screen::active() == false → repaints the avatar; nothing
        // else to do here.
        g_painted = false;
    }
    return true;
}

} // namespace stackchan::app::ap_screen
