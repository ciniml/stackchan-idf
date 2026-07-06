// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <memory>
#include <tl/expected.hpp>

#include <M5GFX.h>
#include <driver/gpio.h>
#include <driver/uart.h>

namespace stackchan::board {

enum class Error {
    PmicInit,
    DisplayInit,
    ExpanderProbe,
    ExpanderWrite,
    TouchProbe,
    TouchRead,
    // espressif/led_strip RMT driver init / blit failure (NekomimiLedStrip).
    // Distinct from ExpanderWrite so callers can tell which strip backend
    // failed when both PY32 and RMT strips coexist.
    LedStripIo,
};

// Which Stack-chan base board the SoC is mounted on. Detected at begin() —
// CoreS3 variants discriminate on the M5 base's PY32 IO expander (0x6F);
// AtomS3R variants ("Atom-nyan") are picked up from M5Unified's chip ID.
enum class BoardKind {
    M5Base,    // M5Stack Stack-chan base: PY32 servo-power EN, INA226 battery, servo on G6/G7.
    TakaoBase, // Takao Base (CoreS3 SE port A): half-duplex servos on port A, no power/battery control.
    AtomNyan,  // AtomS3R + Atomic ECHO BASE: 128x128 LCD, ES8311 codec, no servo/battery/LED/touch.
    AtomS3,    // Plain AtomS3 (no PSRAM) + Atomic ECHO BASE: avatar / jtts / LED only, no conv/audio-stream.
    StopWatch, // M5 StopWatch (C152): 466×466 AMOLED 円形 + CST820B touch + ES8311 + M5PM1 PMIC + M5IOE1.
               // No PY32 / no INA226 / no Si12T / no nekomimi LED 配線. Servo は背面 UART0 経由の Phase 3 オプション.
};

// WIRE-FORMAT CONTRACT: the numeric values above are externally visible —
// BLE chr "BoardKind" serves static_cast<uint8_t>(kind) to paired web UIs,
// and wifi_config_service's release-OTA maps the same byte to a firmware
// slug (cores3/atoms3r/atoms3/stopwatch). Never reorder or renumber; append
// new boards at the end only.
static_assert(static_cast<int>(BoardKind::M5Base) == 0 &&
              static_cast<int>(BoardKind::TakaoBase) == 1 &&
              static_cast<int>(BoardKind::AtomNyan) == 2 &&
              static_cast<int>(BoardKind::AtomS3) == 3 &&
              static_cast<int>(BoardKind::StopWatch) == 4,
              "BoardKind numbering is a BLE/OTA wire contract — append only");

// Per-board static traits, resolved once from the detected BoardKind. This
// collects every "which board am I?" decision that used to live as scattered
// kind() switches across main/ — speaker gain policy, UI flavour, input
// gestures, peripheral presence. Runtime discoveries that cut across the
// board kind (currently only the Module Audio ES8388 probe) are folded in by
// the boot code mutating its profile copy in ONE place (see app_main), so
// downstream consumers never re-derive two-axis decisions.
struct BoardProfile {
    // M5Unified spk_cfg.magnification override; 0 = keep the factory value
    // (M5Unified tunes CoreS3 / AtomNyan per their codec+amp pair).
    // StopWatch needs 16: the ES8311 → AW8737A path has a 200 kΩ input
    // attenuator eating ~-8 dB.
    std::uint8_t speaker_magnification = 0;
    // Per-board factory speaker volume — the "100 %" reference the user
    // gain slider multiplies. Weak downstream paths (StopWatch, Module
    // Audio line-out) ship at full digital scale.
    std::uint8_t speaker_base_volume = 128;
    // Round panel (StopWatch AMOLED): render inside the inscribed circle.
    bool circular_display = false;
    // On-device UI flavour: true = atom_status button overlay (no LCD
    // touch), false = the touch-driven device_ui tab screen.
    bool button_overlay_ui = false;
    // BtnA toggles the device_ui (StopWatch: corner hot-zone on a round
    // panel is awkward, the physical button is the reliable opener).
    bool btn_a_toggles_ui = false;
    // Outer-ring touch drags the avatar's gaze (StopWatch round panel).
    bool touch_gaze_follow = false;
    // SCS servo bus wired (M5/Takao base). Runtime conditions can still
    // revoke it (Module Audio steals G6/G7 — see app_main).
    bool has_servo_bus = false;
    // GC0308 DVP camera wired (CoreS3 mainboard on the M5 base). The
    // CONFIG_STACKCHAN_CAMERA_ENABLED compile gate applies on top.
    bool has_camera = false;
};

// The static profile for a detected board. Pure function of the enum — the
// single place new per-board decisions should land.
BoardProfile profile_for(BoardKind kind) noexcept;

// SCS servo bus wiring for the detected board. Maps 1:1 onto scs_servo::ScsBus::Config.
struct ServoBusConfig {
    uart_port_t uart;
    gpio_num_t tx;
    gpio_num_t rx;
    std::uint32_t baud;
    bool echo_cancel; // Takao's half-duplex bus echoes our TX back onto RX.
};

class Si12tTouch;
class LedStrip;

class Board {
public:
    static tl::expected<Board, Error> begin();

    Board(Board&&) noexcept = default;
    Board& operator=(Board&&) noexcept = default;
    Board(const Board&) = default;
    Board& operator=(const Board&) = default;
    ~Board() = default;

    M5GFX& display() noexcept;

    // Which base board was detected.
    BoardKind kind() const noexcept;
    // SCS servo bus wiring for the detected board.
    ServoBusConfig servo_bus_config() const noexcept;
    // True if this board can report battery level (M5 base = INA226; Takao = no).
    bool has_battery() const noexcept;

    // Enable/disable servo Vmotor. No-op (returns success) on boards without a
    // servo-power control line (Takao base — servos are always powered).
    tl::expected<void, Error> set_servo_power(bool on);

    // Top-mounted Si12T touch sensor. nullptr if the chip didn't probe at
    // boot (e.g. older base hardware without the sensor); callers should
    // null-check before using.
    Si12tTouch* touch_sensor() noexcept;

    // NeoPixel strip on the M5 base back panel (12 × WS2812 driven by the
    // PY32 over I2C). nullptr on the Takao base, which has no strip. Callers
    // should null-check (animation tasks skip themselves when absent).
    LedStrip* led_strip() noexcept;

    // Brief tactile pulse on the vibration motor (StopWatch M5IOE1 PYG9).
    // Blocking — duration_ms ≤ 200 is the intended range. No-op on boards
    // without a motor (returns immediately). Returns true if a pulse was
    // actually fired so callers can attribute haptic feedback in logs.
    bool vibrate(std::uint32_t duration_ms);

private:
    Board() = default;
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace stackchan::board
