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
};

// Which Stack-chan base board the CoreS3 is mounted on. Detected at begin() by
// the presence of the M5 base's PY32 IO expander (0x6F).
enum class BoardKind {
    M5Base,    // M5Stack Stack-chan base: PY32 servo-power EN, INA226 battery, servo on G6/G7.
    TakaoBase, // Takao Base (CoreS3 SE port A): half-duplex servos on port A, no power/battery control.
};

// SCS servo bus wiring for the detected board. Maps 1:1 onto scs_servo::ScsBus::Config.
struct ServoBusConfig {
    uart_port_t uart;
    gpio_num_t tx;
    gpio_num_t rx;
    std::uint32_t baud;
    bool echo_cancel; // Takao's half-duplex bus echoes our TX back onto RX.
};

class Si12tTouch;

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

private:
    Board() = default;
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace stackchan::board
