// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/board.hpp"

#include <memory>
#include <optional>
#include <utility>

#include <M5Unified.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board/io_expander_py32.hpp"
#include "board/led_strip.hpp"
#include "board/nekomimi_led_strip.hpp"
#include "board/si12t_touch.hpp"

namespace stackchan::board {

namespace {
constexpr const char* kTag = "board";
// M5 Stack-chan back-panel NeoPixel count. (BSP exposes setLedCount but doesn't
// hardcode this — the physical strip is 12.)
constexpr std::uint8_t kM5LedCount = 12;
} // namespace

class Board::Impl {
public:
    Impl(BoardKind kind, std::optional<Py32Expander>&& expander,
         std::optional<Si12tTouch>&& touch) noexcept
        : kind_{kind}, expander_{std::move(expander)}, touch_{std::move(touch)}
    {
        // Stack-chan ネコミミ NeoPixel (18 LEDs = 9 per ear) — present on
        // M5/Takao/AtomNyan bases, but the data line varies: CoreS3 uses
        // GPIO9 (free pin near the M-BUS), AtomNyan uses GPIO38 (the
        // available pin on Atomic ECHO BASE's headers). We prefer the
        // nekomimi strip over the M5-base PY32 ring (currently disabled —
        // JOURNAL: "M5 base 背面 NeoPixel … 完全に無効化中"); the PY32
        // path can come back via a separate accessor without disturbing
        // this one. StopWatch (C152) has no nekomimi wiring at all — leave
        // led_ as nullptr so app_main / led_task null-check naturally.
        if (kind_ == BoardKind::StopWatch) {
            return;
        }
        const int gpio = (kind_ == BoardKind::AtomNyan)
                             ? NekomimiLedStrip::kDataGpioAtomNyan
                             : NekomimiLedStrip::kDataGpioCoreS3;
        led_ = std::make_unique<NekomimiLedStrip>(gpio);
    }

    BoardKind kind() const noexcept { return kind_; }
    std::optional<Py32Expander>& expander() noexcept { return expander_; }
    Si12tTouch* touch() noexcept { return touch_ ? &*touch_ : nullptr; }
    LedStrip* led() noexcept { return led_.get(); }

private:
    BoardKind kind_;
    std::optional<Py32Expander> expander_;
    std::optional<Si12tTouch> touch_;
    // Polymorphic strip — owned via unique_ptr because Py32LedStrip and
    // NekomimiLedStrip have different sizes / move semantics. nullptr on
    // hardware without any strip (AtomNyan).
    std::unique_ptr<LedStrip> led_;
};

tl::expected<Board, Error> Board::begin()
{
    auto cfg = M5.config();
    // Always-on Atomic ECHO BASE codec hint. M5Unified only consults this flag
    // inside the AtomS3R-family case branch (M5Unified.cpp:2113), so it's a
    // no-op on CoreS3 and just makes ES8311 init automatic when we boot on an
    // AtomS3R + ECHO BASE combo without us having to detect that beforehand.
    cfg.external_speaker.atomic_echo = 1;
    M5.begin(cfg);

    // AtomS3R / AtomS3 / StopWatch — picked up from M5Unified's board ID
    // after M5.begin. None of these carry the M5 Stack-chan base PY32 / Si12T
    // / INA226 stack so we bail out of those probe phases entirely. Each
    // gets its own BoardKind so settings UIs / build gates can tell them
    // apart (different LCD / capability set).
    const auto m5_board = M5.getBoard();
    const bool is_atom_s3r =
        m5_board == m5::board_t::board_M5AtomS3R    ||
        m5_board == m5::board_t::board_M5AtomS3RExt ||
        m5_board == m5::board_t::board_M5AtomEchoS3R ||
        m5_board == m5::board_t::board_M5AtomS3RCam;
    const bool is_atom_s3 = m5_board == m5::board_t::board_M5AtomS3;
    const bool is_stopwatch = m5_board == m5::board_t::board_M5StopWatch;
    if (is_stopwatch) {
        // StopWatch (C152): 466×466 AMOLED 円形パネル。
        // M5.begin() で Panel_CO5300 autodetect / CST820B touch / ES8311
        // codec / M5PM1 power / RX8130CE RTC が立ち上がる (M5Unified ≥ 0.2.17
        // / M5GFX ≥ 0.2.23)。回転は 0 (USB-C 上、表示は AMOLED native 方位)。
        // 円形なので setRotation の数値は表示の見え方には影響しない (アクティブ
        // ピクセルが正方形なので 90° 倍数ならどれでも収まる) が、後段 UI が
        // 矩形扱いするので 0 に固定。
        M5.Display.setRotation(0);
        // Force the ES8311 enable callback NOW (rather than waiting for the
        // first lazy playRaw downstream). app_main runs a startup arpeggio
        // around line 810 — if Speaker.begin() hasn't completed by then,
        // the tone goes out before the M5IOE1 audio power / PA / codec
        // init sequence finishes and we hear nothing. The 200 ms delay
        // gives the ES8311 + M5IOE1 power rail time to settle (the
        // M5Unified callback only delays 10 ms after Audio Power ON before
        // writing ES8311 registers, which experimentally was too tight on
        // the bench unit). Logging the result so a future regression on
        // the M5IOE1 / codec bring-up is obvious from the boot trace.
        {
            const bool ok = M5.Speaker.begin();
            ESP_LOGI(kTag, "StopWatch M5.Speaker.begin -> %s", ok ? "ok" : "FAILED");
            vTaskDelay(pdMS_TO_TICKS(200));
            // Volume is set by app_main per board (StopWatch needs max because
            // M5Unified ships spk_cfg.magnification=1 on this hardware). Leave
            // it default here.
        }
        Board board;
        board.impl_ = std::make_shared<Impl>(BoardKind::StopWatch,
                                             std::optional<Py32Expander>{},
                                             std::optional<Si12tTouch>{});
        // StopWatch には nekomimi NeoPixel の専用配線が無いので LED strip は
        // 初期化しない (Impl の led_ は構築時に nullptr 相当 — 後述、別途
        // 直し)。app_main / led_task は led == nullptr を null-check で
        // skip するので動作には影響しない。
        ESP_LOGI(kTag, "board initialized: kind=StopWatch (466×466 AMOLED, M5PM1, no PY32/Si12T/nekomimi)");
        return board;
    }
    if (is_atom_s3 || is_atom_s3r) {
        const BoardKind kind = is_atom_s3 ? BoardKind::AtomS3 : BoardKind::AtomNyan;
        // AtomS3 / AtomS3R: square 128x128 GC9107, base rotation (0) lands
        // the way the user holds the device. The CoreS3 default of 1 came
        // up 90° CW, and 3 ended up at the same wrong orientation (only
        // 0 / 2 differ on this panel's mapping) — 0 was the correct one.
        M5.Display.setRotation(0);
        Board board;
        board.impl_ = std::make_shared<Impl>(kind,
                                             std::optional<Py32Expander>{},
                                             std::optional<Si12tTouch>{});
        // Initialise the nekomimi LED strip on this path too. The CoreS3
        // branch below shares one led->begin() call; the Atom branch would
        // otherwise skip it and the GPIO38 chain never lights up (the RMT
        // channel / WS2812 encoder are allocated lazily on begin()).
        if (LedStrip* led = board.impl_->led(); led != nullptr) {
            if (auto r = led->begin(); !r) {
                ESP_LOGW(kTag, "LED strip begin failed (Atom): %d",
                         static_cast<int>(r.error()));
            }
        }
        ESP_LOGI(kTag, "board initialized: kind=%s",
                 is_atom_s3 ? "AtomS3 (no PSRAM, slim profile)"
                            : "AtomNyan (AtomS3R + Atomic ECHO BASE)");
        return board;
    }
    // CoreS3: landscape, USB-C on the right edge.
    M5.Display.setRotation(1);

    // CoreS3 variants — discriminate M5 base vs Takao base by probing the PY32
    // IO expander at 0x6F (M5 base only). PY32 can take up to ~1.2 s after a
    // cold reset, so poll once every 200 ms for ~1.2 s before deciding.
    std::optional<Py32Expander> expander;
    for (int attempt = 0; attempt < 6 && !expander; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (auto e = Py32Expander::probe(); e) {
            expander.emplace(std::move(*e));
        }
    }

    BoardKind kind = expander ? BoardKind::M5Base : BoardKind::TakaoBase;
    if (kind == BoardKind::M5Base) {
        // Configure the servo-power EN pin (start OFF). A failure here is fatal
        // on the M5 base since the servos would never get power.
        if (auto r = expander->set_direction(Py32Expander::kPinServoPowerEnable, /*output=*/true); !r)
            return tl::unexpected{r.error()};
        if (auto r = expander->set_pull_up(Py32Expander::kPinServoPowerEnable, true); !r)
            return tl::unexpected{r.error()};
        if (auto r = expander->digital_write(Py32Expander::kPinServoPowerEnable, false); !r)
            return tl::unexpected{r.error()};
    } else {
        ESP_LOGI(kTag, "PY32 not found at 0x%02X -> Takao base (no servo-power / battery control)",
                 Py32Expander::kAddress);
    }

    // Top-mounted touch sensor (Si12T at 0x68). Optional on either base — boot
    // anyway and just warn if it doesn't respond.
    std::optional<Si12tTouch> touch;
    if (auto t = Si12tTouch::probe(); t) {
        touch.emplace(std::move(*t));
    } else {
        ESP_LOGW(kTag, "Si12T touch sensor not found at 0x%02X", Si12tTouch::kAddress);
    }

    Board board;
    board.impl_ = std::make_shared<Impl>(kind, std::move(expander), std::move(touch));
    // LED strip init. Earlier attempts left the strip dark because the host-
    // side data format was wrong (3-byte GRB per LED). The PY32 firmware
    // actually expects 2-byte RGB565 little-endian per LED — see
    // docs/py32_ioexpander.md §6. The separate LCD-backlight blackout incident
    // came from an I2C scan touching AXP2101 (0x34); the scan code is no
    // longer in the tree and accessing 0x34 is forbidden — see CLAUDE.md /
    // the docs §1.
    LedStrip* led = board.impl_->led();
    if (led != nullptr) {
        if (auto r = led->begin(); !r) {
            ESP_LOGW(kTag, "LED strip begin failed: %d", static_cast<int>(r.error()));
        }
    }
    ESP_LOGI(kTag, "board initialized: kind=%s (servo power: OFF, leds: %s)",
             kind == BoardKind::M5Base ? "M5Base" : "TakaoBase",
             led != nullptr ? "ready" : "none");
    return board;
}

M5GFX& Board::display() noexcept
{
    return M5.Display;
}

BoardKind Board::kind() const noexcept
{
    return impl_->kind();
}

BoardProfile profile_for(BoardKind kind) noexcept
{
    BoardProfile p{};
    switch (kind) {
    case BoardKind::M5Base:
        p.has_servo_bus = true;
        p.has_camera = true; // GC0308 on the CoreS3 mainboard
        break;
    case BoardKind::TakaoBase:
        p.has_servo_bus = true;
        // CoreS3 SE (the usual Takao host) has no camera; keep it off until
        // a camera-equipped Takao build shows up.
        break;
    case BoardKind::AtomNyan:
    case BoardKind::AtomS3:
        p.button_overlay_ui = true; // 128×128, no LCD touch → atom_status
        break;
    case BoardKind::StopWatch:
        p.speaker_magnification = 16; // ES8311 → AW8737A input attenuator
        p.speaker_base_volume = 255;  // weak downstream path, full scale
        p.circular_display = true;
        p.btn_a_toggles_ui = true;
        p.touch_gaze_follow = true;
        break;
    }
    return p;
}

ServoBusConfig Board::servo_bus_config() const noexcept
{
    if (impl_->kind() == BoardKind::AtomNyan ||
        impl_->kind() == BoardKind::AtomS3 ||
        impl_->kind() == BoardKind::StopWatch) {
        // No on-board servo bus on these variants — the servo task is not
        // started. Return a syntactically valid but inert config so callers
        // that read this defensively don't see junk. (StopWatch *can* drive
        // an external servo via the back-side 2.54 mm bus UART0=G43/G44, but
        // that's a Phase-3 opt-in route; default profile keeps it inert.)
        return {UART_NUM_1, GPIO_NUM_NC, GPIO_NUM_NC, 0u, /*echo_cancel=*/false};
    }
    if (impl_->kind() == BoardKind::TakaoBase) {
        // Takao base on CoreS3 port A: TX=G2, RX=G1. (Port A naming wasn't a
        // reliable guide to which physical pin carries which signal; this
        // assignment was found by bring-up — the reverse order leaves RX
        // unable to see the servo's reply.) Half-duplex bus: TX drives the
        // bus through a series diode that isolates our push-pull driver from
        // the bus when idle-high, and the line echoes our bytes back onto RX.
        return {UART_NUM_1, GPIO_NUM_2, GPIO_NUM_1, 1'000'000u, /*echo_cancel=*/true};
    }
    // M5 base: dedicated servo UART on G6/G7, no echo.
    return {UART_NUM_1, GPIO_NUM_6, GPIO_NUM_7, 1'000'000u, /*echo_cancel=*/false};
}

bool Board::has_battery() const noexcept
{
    // M5 base: INA226. StopWatch: M5PM1 PMIC (M5Unified `M5.Power` API
    // surfaces battery voltage / state-of-charge directly). Other variants
    // have no battery telemetry path we expose here.
    return impl_->kind() == BoardKind::M5Base ||
           impl_->kind() == BoardKind::StopWatch;
}

tl::expected<void, Error> Board::set_servo_power(bool on)
{
    auto& expander = impl_->expander();
    if (!expander) {
        // Takao base / Atom family / StopWatch: no PY32 servo-power
        // control line — servos (if any) are externally powered. No-op
        // success keeps the boot sequence simple.
        return {};
    }
    return expander->digital_write(Py32Expander::kPinServoPowerEnable, on);
}

Si12tTouch* Board::touch_sensor() noexcept
{
    return impl_->touch();
}

LedStrip* Board::led_strip() noexcept
{
    return impl_->led();
}

bool Board::vibrate(std::uint32_t duration_ms)
{
    // StopWatch only: vibration motor wired to M5IOE1 PYG9. Other boards
    // have no motor and return immediately. Direct I2C poke on the M5IOE1
    // (addr 0x4F) — M5Unified configures PYG10 (PA) on the same output
    // register 0x06 at boot, so register 0x04 (direction) bit 0 for PYG9
    // must be set to output first. We do that lazily here (cheap I2C write
    // each call; vibrate is rare). Stays atomic w.r.t. M5Unified's own
    // M5IOE1 traffic because bit-OR-ing into 0x04 / 0x14 is read-modify-
    // write through m5::In_I2C which serializes on its internal mutex.
    if (impl_->kind() != BoardKind::StopWatch) return false;
    if (duration_ms == 0) return false;
    if (duration_ms > 1000) duration_ms = 1000; // upper bound
    constexpr std::uint8_t kM5ioe1Addr = 0x4F;
    constexpr std::uint32_t kI2cFreq = 100'000;
    // direction = output (reg 0x04 bit 0 = 1).
    m5::In_I2C.bitOn(kM5ioe1Addr, 0x04, 0b00000001, kI2cFreq);
    // input enable = off (reg 0x14 bit 0 = 0).
    m5::In_I2C.bitOff(kM5ioe1Addr, 0x14, 0b00000001, kI2cFreq);
    // output high (reg 0x06 bit 0).
    m5::In_I2C.bitOn(kM5ioe1Addr, 0x06, 0b00000001, kI2cFreq);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    m5::In_I2C.bitOff(kM5ioe1Addr, 0x06, 0b00000001, kI2cFreq);
    return true;
}

} // namespace stackchan::board
