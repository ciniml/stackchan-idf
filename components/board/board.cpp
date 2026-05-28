// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/board.hpp"

#include <optional>
#include <utility>

#include <M5Unified.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board/io_expander_py32.hpp"
#include "board/si12t_touch.hpp"

namespace stackchan::board {

namespace {
constexpr const char* kTag = "board";
} // namespace

class Board::Impl {
public:
    Impl(BoardKind kind, std::optional<Py32Expander>&& expander,
         std::optional<Si12tTouch>&& touch) noexcept
        : kind_{kind}, expander_{std::move(expander)}, touch_{std::move(touch)}
    {
    }

    BoardKind kind() const noexcept { return kind_; }
    std::optional<Py32Expander>& expander() noexcept { return expander_; }
    Si12tTouch* touch() noexcept { return touch_ ? &*touch_ : nullptr; }

private:
    BoardKind kind_;
    std::optional<Py32Expander> expander_;
    std::optional<Si12tTouch> touch_;
};

tl::expected<Board, Error> Board::begin()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);

    // Base-board detection: the M5 Stack-chan base carries a PY32 IO expander at
    // 0x6F (servo-power EN); the Takao base has none. PY32 can take up to ~1.2 s
    // after a cold reset, so poll once every 200 ms for ~1.2 s before deciding.
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
    ESP_LOGI(kTag, "board initialized: kind=%s (servo power: OFF)",
             kind == BoardKind::M5Base ? "M5Base" : "TakaoBase");
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

ServoBusConfig Board::servo_bus_config() const noexcept
{
    if (impl_->kind() == BoardKind::TakaoBase) {
        // Takao base on CoreS3 port A (G1/G2), half-duplex (TX echoes onto RX).
        // TX/RX assignment is provisional — verify against the Takao wiring on
        // hardware bring-up (swap, or use a single shared pin, if needed).
        return {UART_NUM_1, GPIO_NUM_1, GPIO_NUM_2, 1'000'000u, /*echo_cancel=*/true};
    }
    // M5 base: dedicated servo UART on G6/G7, no echo.
    return {UART_NUM_1, GPIO_NUM_6, GPIO_NUM_7, 1'000'000u, /*echo_cancel=*/false};
}

bool Board::has_battery() const noexcept
{
    return impl_->kind() == BoardKind::M5Base; // INA226 only on the M5 base
}

tl::expected<void, Error> Board::set_servo_power(bool on)
{
    auto& expander = impl_->expander();
    if (!expander) {
        return {}; // Takao base: no servo-power control line — always powered.
    }
    return expander->digital_write(Py32Expander::kPinServoPowerEnable, on);
}

Si12tTouch* Board::touch_sensor() noexcept
{
    return impl_->touch();
}

} // namespace stackchan::board
