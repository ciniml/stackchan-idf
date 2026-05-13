#include "render_task.hpp"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "avatar/avatar.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "render";
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(33);

void render_task_entry(void* arg)
{
    auto& args = *static_cast<RenderTaskArgs*>(arg);

    avatar::Avatar avatar{*args.display};
    if (!avatar.begin()) {
        ESP_LOGE(kTag, "avatar.begin() failed");
        vTaskDelete(nullptr);
        return;
    }

    int last_expression = -1;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const int expr = args.state->expression.load(std::memory_order_relaxed);
        if (expr != last_expression) {
            avatar.set_expression(static_cast<avatar::Expression>(expr));
            last_expression = expr;
        }
        avatar.set_mouth_open(args.state->mouth_open.load(std::memory_order_relaxed));

        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        avatar.tick(now_ms);

        vTaskDelayUntil(&last_wake, kPeriodTicks);
    }
}

} // namespace

void start_render_task(RenderTaskArgs& args)
{
    xTaskCreatePinnedToCore(render_task_entry, "render", 8192, &args, 5, nullptr, 1);
}

} // namespace stackchan::app
