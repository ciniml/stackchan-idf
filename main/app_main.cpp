#include <cmath>
#include <cstdint>

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "avatar/expression.hpp"
#include "board/board.hpp"
#include "render_task.hpp"
#include "servo_task.hpp"
#include "shared_state.hpp"

namespace {

constexpr const char* kTag = "stackchan";

// Heap-allocate so the task argument outlives app_main's scope (the tasks run forever).
stackchan::app::SharedState* g_state = nullptr;
stackchan::app::RenderTaskArgs* g_render_args = nullptr;
stackchan::app::ServoTaskArgs* g_servo_args = nullptr;

void demo_loop()
{
    using namespace stackchan;

    constexpr avatar::Expression kCycle[] = {
        avatar::Expression::Neutral, avatar::Expression::Happy, avatar::Expression::Doubt,
        avatar::Expression::Sad,     avatar::Expression::Angry, avatar::Expression::Sleepy,
    };

    // Random head pose targets, redrawn every kPoseMinMs..kPoseMaxMs.
    constexpr float kYawMaxDeg = 40.0f;          // ±40°
    constexpr float kPitchMinDeg = -10.0f;       // little down
    constexpr float kPitchMaxDeg =  25.0f;       // more up
    constexpr std::uint32_t kPoseMinMs = 10000;
    constexpr std::uint32_t kPoseMaxMs = 20000;
    constexpr std::uint32_t kExpressionPeriodMs = 5000;

    auto rand_in = [](float low, float high) {
        const float u = static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX);
        return low + (high - low) * u;
    };
    auto rand_range_ms = [](std::uint32_t low, std::uint32_t high) {
        return low + (esp_random() % (high - low + 1));
    };

    std::size_t expression_index = 0;
    std::uint32_t next_expression_ms = 0;
    std::uint32_t next_pose_ms = 0;

    for (;;) {
        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);

        // Mouth: smooth bobble at ~0.5 Hz drives only the avatar, not a servo,
        // so it's safe to update continuously.
        const float t = static_cast<float>(now_ms) / 1000.0f;
        const float mouth = 0.5f + 0.5f * std::sin(t * 2.0f * 3.14159265f / 2.0f);
        g_state->mouth_open.store(mouth, std::memory_order_relaxed);

        // Random yaw + pitch every 10–20 s.
        if (now_ms >= next_pose_ms) {
            g_state->target_yaw_deg.store(rand_in(-kYawMaxDeg, kYawMaxDeg), std::memory_order_relaxed);
            g_state->target_pitch_deg.store(rand_in(kPitchMinDeg, kPitchMaxDeg), std::memory_order_relaxed);
            next_pose_ms = now_ms + rand_range_ms(kPoseMinMs, kPoseMaxMs);
        }

        // Cycle expression every 5 s.
        if (now_ms >= next_expression_ms) {
            g_state->expression.store(static_cast<int>(kCycle[expression_index]), std::memory_order_relaxed);
            expression_index = (expression_index + 1) % (sizeof(kCycle) / sizeof(kCycle[0]));
            next_expression_ms = now_ms + kExpressionPeriodMs;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace

extern "C" void app_main()
{
    auto board_result = stackchan::board::Board::begin();
    if (!board_result) {
        ESP_LOGE(kTag, "Board::begin() failed: %d", static_cast<int>(board_result.error()));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    auto& board = *board_result;

    // Quick audio sanity check: a short rising arpeggio so we can hear
    // immediately whether the speaker is wired up correctly.
    M5.Speaker.setVolume(128);
    for (float freq : {523.25f, 659.25f, 783.99f}) { // C5 – E5 – G5
        M5.Speaker.tone(freq, 150);
        vTaskDelay(pdMS_TO_TICKS(180));
    }

    if (auto r = board.set_servo_power(true); !r) {
        ESP_LOGE(kTag, "set_servo_power(true) failed: %d", static_cast<int>(r.error()));
    }
    // Allow the servo bus rail to settle before the servo task starts driving UART.
    vTaskDelay(pdMS_TO_TICKS(500));

    g_state = new stackchan::app::SharedState{};
    g_render_args = new stackchan::app::RenderTaskArgs{.display = &board.display(), .state = g_state};
    g_servo_args = new stackchan::app::ServoTaskArgs{.state = g_state};

    stackchan::app::start_render_task(*g_render_args);
    stackchan::app::start_servo_task(*g_servo_args);

    ESP_LOGI(kTag, "ready");
    demo_loop();
}
