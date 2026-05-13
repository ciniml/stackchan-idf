// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "servo_task.hpp"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "scs_servo/scs_bus.hpp"
#include "scs_servo/scs_servo.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "servo";
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(20);

// SCS0009 Goal Speed is roughly in 0.146°/s units; 200 ≈ 30°/s — pleasant
// head-turning speed. Goal Time = 0 means "use Goal Speed".
constexpr std::uint16_t kGoalSpeed = 200;
constexpr std::uint16_t kGoalTime = 0;

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

    scs_servo::ScsServo yaw{bus, scs_servo::kYawId};
    scs_servo::ScsServo pitch{bus, scs_servo::kPitchId};

    if (auto r = yaw.ping(); !r) {
        ESP_LOGW(kTag, "yaw (id=%u) ping failed: %d", scs_servo::kYawId,
                 static_cast<int>(r.error()));
    } else {
        ESP_LOGI(kTag, "yaw (id=%u) ping OK", scs_servo::kYawId);
    }
    if (auto r = pitch.ping(); !r) {
        ESP_LOGW(kTag, "pitch (id=%u) ping failed: %d", scs_servo::kPitchId,
                 static_cast<int>(r.error()));
    } else {
        ESP_LOGI(kTag, "pitch (id=%u) ping OK", scs_servo::kPitchId);
    }
    if (auto r = yaw.enable_torque(true); !r) {
        ESP_LOGW(kTag, "yaw enable_torque failed: %d", static_cast<int>(r.error()));
    }
    if (auto r = pitch.enable_torque(true); !r) {
        ESP_LOGW(kTag, "pitch enable_torque failed: %d", static_cast<int>(r.error()));
    }

    std::uint16_t last_yaw_target = scs_servo::kYawZero;
    std::uint16_t last_pitch_target = scs_servo::kPitchZero;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const float yaw_deg = args.state->target_yaw_deg.load(std::memory_order_relaxed);
        const float pitch_deg = args.state->target_pitch_deg.load(std::memory_order_relaxed);
        const std::uint16_t yaw_target = scs_servo::deg_to_raw(yaw_deg, scs_servo::kYawZero);
        const std::uint16_t pitch_target = scs_servo::deg_to_raw(pitch_deg, scs_servo::kPitchZero);

        if (yaw_target != last_yaw_target) {
            (void)yaw.write_goal_position(yaw_target, kGoalTime, kGoalSpeed);
            last_yaw_target = yaw_target;
        }
        if (pitch_target != last_pitch_target) {
            (void)pitch.write_goal_position(pitch_target, kGoalTime, kGoalSpeed);
            last_pitch_target = pitch_target;
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
