// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "servo_task.hpp"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
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
    // Torque is engaged on demand: enabled just before a move, released once
    // the move should have completed, so the servos are silent / cool / free
    // while the head holds still. Moves are speed-based (kGoalTime = 0), so we
    // estimate the duration from the angular distance and goal speed. The
    // on-device 操作 toggle's servo_enabled = false forces torque off and
    // suppresses moves entirely (脱力).
    constexpr float kDegPerStep = 0.3125f;      // SCS0009: 1 step ≈ 0.3125°
    constexpr float kDegPerSpeedUnit = 0.15f;   // goal-speed unit ≈ 0.15 °/s
    constexpr std::uint32_t kSettleMarginMs = 150; // hold a bit past the estimate
    constexpr std::uint32_t kMinHoldMs = 120;
    constexpr std::uint32_t kUnknownMoveMs = 1500;  // start pose / re-drive (origin unknown)

    auto move_ms = [](std::uint16_t from, std::uint16_t to, std::uint16_t speed) -> std::uint32_t {
        const int delta = (from > to) ? (from - to) : (to - from);
        const float dps = speed * kDegPerSpeedUnit;
        if (dps <= 0.0f) return kMinHoldMs;
        const std::uint32_t ms = static_cast<std::uint32_t>(delta * kDegPerStep / dps * 1000.0f);
        return ms < kMinHoldMs ? kMinHoldMs : ms;
    };
    auto now_ms = [] { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); };

    // Start with torque off; the first loop drives to the commanded pose.
    std::uint16_t last_yaw_target = 0xFFFF;
    std::uint16_t last_pitch_target = 0xFFFF;
    bool torque_on = false;
    bool last_enabled = true;
    std::uint32_t release_at = 0; // when to drop torque after the current move

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const bool enabled = args.state->servo_enabled.load(std::memory_order_relaxed);
        if (!enabled) {
            if (torque_on) {
                (void)yaw.enable_torque(false);
                (void)pitch.enable_torque(false);
                torque_on = false;
            }
            last_enabled = false;
            vTaskDelayUntil(&last_wake, kPeriodTicks);
            continue;
        }
        if (!last_enabled) {
            // Re-enabled (復帰): re-drive to the commanded pose.
            last_yaw_target = last_pitch_target = 0xFFFF;
            last_enabled = true;
        }

        const float yaw_deg = args.state->target_yaw_deg.load(std::memory_order_relaxed);
        const float pitch_deg = args.state->target_pitch_deg.load(std::memory_order_relaxed);
        const std::uint16_t yaw_target = scs_servo::deg_to_raw(yaw_deg, scs_servo::kYawZero);
        const std::uint16_t pitch_target = scs_servo::deg_to_raw(pitch_deg, scs_servo::kPitchZero);

        // Non-zero servo_speed_override lets the demo task drive snappy
        // gestures (e.g. head shake on nadenade) without permanently raising
        // the default head-turn speed.
        const std::uint16_t override = args.state->servo_speed_override.load(std::memory_order_relaxed);
        const std::uint16_t speed = override != 0 ? override : kGoalSpeed;

        if (yaw_target != last_yaw_target || pitch_target != last_pitch_target) {
            // Engage torque just before driving.
            if (!torque_on) {
                (void)yaw.enable_torque(true);
                (void)pitch.enable_torque(true);
                torque_on = true;
            }
            std::uint32_t mv = kMinHoldMs;
            if (yaw_target != last_yaw_target) {
                const std::uint32_t a = (last_yaw_target == 0xFFFF)
                                            ? kUnknownMoveMs
                                            : move_ms(last_yaw_target, yaw_target, speed);
                if (a > mv) mv = a;
                (void)yaw.write_goal_position(yaw_target, kGoalTime, speed);
                last_yaw_target = yaw_target;
            }
            if (pitch_target != last_pitch_target) {
                const std::uint32_t a = (last_pitch_target == 0xFFFF)
                                            ? kUnknownMoveMs
                                            : move_ms(last_pitch_target, pitch_target, speed);
                if (a > mv) mv = a;
                (void)pitch.write_goal_position(pitch_target, kGoalTime, speed);
                last_pitch_target = pitch_target;
            }
            release_at = now_ms() + mv + kSettleMarginMs;
        } else if (torque_on && now_ms() >= release_at) {
            // Move complete and holding still → release torque.
            (void)yaw.enable_torque(false);
            (void)pitch.enable_torque(false);
            torque_on = false;
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
