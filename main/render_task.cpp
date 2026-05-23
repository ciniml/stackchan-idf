// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "render_task.hpp"

#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "avatar/avatar.hpp"
#include "device_ui.hpp"

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
    std::uint32_t last_balloon_version = 0;
    std::string balloon_scratch;
    bool balloon_pending = false;
    bool ui_was_active = false;

    for (;;) {
        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);

        // On-device touchscreen UI takes over the display while shown.
        if (ui::active()) {
            ui::draw(*args.display);
            ui_was_active = true;
            vTaskDelay(kPeriodTicks);
            continue;
        }
        if (ui_was_active) {
            // Returning to the avatar — its pushSprite() repaints the whole
            // screen on the next tick(), overwriting the UI.
            ui_was_active = false;
            last_expression = -1; // force a fresh expression apply
        }

        const int expr = args.state->expression.load(std::memory_order_relaxed);
        if (expr != last_expression) {
            avatar.set_expression(static_cast<avatar::Expression>(expr));
            last_expression = expr;
        }
        avatar.set_mouth_open(args.state->mouth_open.load(std::memory_order_relaxed));

        const std::uint32_t balloon_version = args.state->balloon_version();
        if (balloon_version != last_balloon_version) {
            if (args.state->balloon_visible()) {
                std::uint32_t hold_ms = 0;
                args.state->snapshot_balloon(balloon_scratch, hold_ms);
                avatar.set_balloon_text(balloon_scratch, hold_ms);
                balloon_pending = true;
            } else {
                avatar.clear_balloon();
                balloon_pending = false;
            }
            last_balloon_version = balloon_version;
        }

        avatar.tick(now_ms);

        if (balloon_pending && avatar.is_balloon_done()) {
            balloon_pending = false;
            args.state->notify_balloon_complete();
        }

        // Use vTaskDelay (not vTaskDelayUntil) so the IDLE task on this core
        // always gets at least one tick, even if a frame ran long.
        vTaskDelay(kPeriodTicks);
    }
}

} // namespace

void start_render_task(RenderTaskArgs& args)
{
    xTaskCreatePinnedToCore(render_task_entry, "render", 8192, &args, 5, nullptr, 1);
}

} // namespace stackchan::app
