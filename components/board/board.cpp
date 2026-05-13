#include "board/board.hpp"

#include <M5Unified.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board/io_expander_py32.hpp"

namespace stackchan::board {

namespace {
constexpr const char* kTag = "board";
} // namespace

class Board::Impl {
public:
    explicit Impl(Py32Expander&& expander) noexcept : expander_{std::move(expander)} {}

    Py32Expander& expander() noexcept { return expander_; }

private:
    Py32Expander expander_;
};

std::expected<Board, Error> Board::begin()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);

    // PY32 can take up to ~1.2 s to come up after a cold reset. Match the
    // BSP's polling pattern: try once every 200 ms for ~1.2 s.
    std::expected<Py32Expander, Error> expander = std::unexpected{Error::ExpanderProbe};
    for (int attempt = 0; attempt < 6; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(200));
        expander = Py32Expander::probe();
        if (expander) {
            break;
        }
    }
    if (!expander) {
        ESP_LOGE(kTag, "PY32 IO expander probe failed at 0x%02X", Py32Expander::kAddress);
        return std::unexpected{expander.error()};
    }

    if (auto r = expander->set_direction(Py32Expander::kPinServoPowerEnable, /*output=*/true); !r) {
        return std::unexpected{r.error()};
    }
    if (auto r = expander->set_pull_up(Py32Expander::kPinServoPowerEnable, true); !r) {
        return std::unexpected{r.error()};
    }
    if (auto r = expander->digital_write(Py32Expander::kPinServoPowerEnable, false); !r) {
        return std::unexpected{r.error()};
    }

    Board board;
    board.impl_ = std::make_shared<Impl>(std::move(*expander));
    ESP_LOGI(kTag, "board initialized (servo power: OFF)");
    return board;
}

M5GFX& Board::display() noexcept
{
    return M5.Display;
}

std::expected<void, Error> Board::set_servo_power(bool on)
{
    return impl_->expander().digital_write(Py32Expander::kPinServoPowerEnable, on);
}

} // namespace stackchan::board
