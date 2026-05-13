#include "servo_task.hpp"

#include <cmath>
#include <cstdio>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "scs_servo/path_generator.hpp"
#include "scs_servo/scs_bus.hpp"
#include "scs_servo/scs_servo.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "servo";
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(20);

// PathGenerator units are raw SCS0009 ticks. Scale these as you tune motion.
constexpr float kMaxAcceleration = 0.5f; // raw ticks per (20 ms)^2
constexpr float kMaxVelocity = 6.0f;     // raw ticks per 20 ms

void servo_task_entry(void* arg)
{
    auto& args = *static_cast<ServoTaskArgs*>(arg);

    scs_servo::ScsBus::Config bus_cfg{
        .uart = UART_NUM_1,
        .tx = GPIO_NUM_6,
        .rx = GPIO_NUM_7,
        .baud = 1'000'000,
        .timeout_ms = 20,
    };
    auto bus_result = scs_servo::ScsBus::create(bus_cfg);
    if (!bus_result) {
        ESP_LOGE(kTag, "ScsBus::create failed: %d", static_cast<int>(bus_result.error()));
        vTaskDelete(nullptr);
        return;
    }
    auto bus = std::move(*bus_result);

    // Diagnostic: scan IDs 1..20 to find what's on the bus.
    {
        char line[96];
        std::size_t pos = std::snprintf(line, sizeof(line), "SCS bus scan responders:");
        for (std::uint8_t id = 1; id <= 20; ++id) {
            scs_servo::ScsServo probe{bus, id};
            if (probe.ping()) {
                pos += std::snprintf(line + pos, sizeof(line) - pos, " %u", id);
            }
        }
        ESP_LOGI(kTag, "%s", line);
    }

    scs_servo::ScsServo yaw{bus, scs_servo::kYawId};
    scs_servo::ScsServo pitch{bus, scs_servo::kPitchId};

    if (auto r = yaw.ping(); !r) {
        ESP_LOGW(kTag, "yaw (id=%u) ping failed: %d", scs_servo::kYawId, static_cast<int>(r.error()));
    } else {
        ESP_LOGI(kTag, "yaw (id=%u) ping OK", scs_servo::kYawId);
    }
    if (auto r = pitch.ping(); !r) {
        ESP_LOGW(kTag, "pitch (id=%u) ping failed: %d", scs_servo::kPitchId, static_cast<int>(r.error()));
    } else {
        ESP_LOGI(kTag, "pitch (id=%u) ping OK", scs_servo::kPitchId);
    }
    (void)yaw.enable_torque(true);
    (void)pitch.enable_torque(true);

    scs_servo::PathGenerator<256> yaw_path{scs_servo::kYawZero, kMaxAcceleration, kMaxVelocity};
    scs_servo::PathGenerator<256> pitch_path{scs_servo::kPitchZero, kMaxAcceleration, kMaxVelocity};

    std::uint16_t last_yaw_target = scs_servo::kYawZero;
    std::uint16_t last_pitch_target = scs_servo::kPitchZero;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const float yaw_deg = args.state->target_yaw_deg.load(std::memory_order_relaxed);
        const float pitch_deg = args.state->target_pitch_deg.load(std::memory_order_relaxed);
        const std::uint16_t yaw_target = scs_servo::deg_to_raw(yaw_deg, scs_servo::kYawZero);
        const std::uint16_t pitch_target = scs_servo::deg_to_raw(pitch_deg, scs_servo::kPitchZero);

        if (yaw_target != last_yaw_target) {
            yaw_path.begin_move_to(yaw_target);
            last_yaw_target = yaw_target;
        }
        if (pitch_target != last_pitch_target) {
            pitch_path.begin_move_to(pitch_target);
            last_pitch_target = pitch_target;
        }

        if (yaw_path.is_moving()) {
            const std::uint16_t pos = static_cast<std::uint16_t>(yaw_path.step_next());
            (void)yaw.write_goal_position(pos, 0, 0);
        }
        if (pitch_path.is_moving()) {
            const std::uint16_t pos = static_cast<std::uint16_t>(pitch_path.step_next());
            (void)pitch.write_goal_position(pos, 0, 0);
        }

        vTaskDelayUntil(&last_wake, kPeriodTicks);
    }
}

} // namespace

void start_servo_task(ServoTaskArgs& args)
{
    xTaskCreatePinnedToCore(servo_task_entry, "servo", 8192, &args, 4, nullptr, 0);
}

} // namespace stackchan::app
