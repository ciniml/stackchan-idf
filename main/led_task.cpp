// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "led_task.hpp"

#include <cmath>
#include <cstdint>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "led";
// 30 Hz is plenty for breathing / rainbow — the 12-LED I2C burst is ~3 ms so
// even at this rate the bus is idle most of the time.
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(33);

constexpr std::uint8_t kModeOff = 0;
constexpr std::uint8_t kModeSolid = 1;
constexpr std::uint8_t kModeBreath = 2;
constexpr std::uint8_t kModeGradient = 3;

// Map a hue in [0, 1) → 24-bit RGB. Standard piecewise sextant HSV with S=V=1.
// Used by the gradient mode.
void hsv_to_rgb(float h, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) noexcept
{
    h -= std::floor(h);
    const float h6 = h * 6.0f;
    const int sector = static_cast<int>(h6);
    const float f = h6 - sector;
    const std::uint8_t v = 255;
    const std::uint8_t p = 0;
    const std::uint8_t q = static_cast<std::uint8_t>(255.0f * (1.0f - f));
    const std::uint8_t t = static_cast<std::uint8_t>(255.0f * f);
    switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

// 8-bit channel × 8-bit gain → 8-bit (rounding away from 0 isn't worth the
// cycles here — the strip can't resolve sub-LSB differences anyway).
inline std::uint8_t scale8(std::uint8_t c, std::uint8_t gain) noexcept
{
    return static_cast<std::uint8_t>((static_cast<std::uint16_t>(c) * gain) / 255);
}

void led_task_entry(void* arg)
{
    auto& args = *static_cast<LedTaskArgs*>(arg);
    auto& strip = *args.strip;
    auto& state = *args.state;
    const std::size_t n = strip.size();
    if (n == 0) {
        ESP_LOGW(kTag, "strip size = 0, exiting");
        vTaskDelete(nullptr);
        return;
    }

    // Frame counter — drives breath phase and gradient scroll. Using a wall-
    // clock-derived value (esp_timer) instead of a frame index keeps animations
    // running at the right speed even if the task ever gets paused / preempted.
    auto now_ms = [] { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); };

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const std::uint8_t mode = state.led_mode.load(std::memory_order_relaxed);
        const std::uint32_t color = state.led_color.load(std::memory_order_relaxed);
        const std::uint8_t bright = state.led_brightness.load(std::memory_order_relaxed);
        const std::uint8_t cr = static_cast<std::uint8_t>((color >> 16) & 0xFF);
        const std::uint8_t cg = static_cast<std::uint8_t>((color >>  8) & 0xFF);
        const std::uint8_t cb = static_cast<std::uint8_t>( color        & 0xFF);

        const float t = now_ms() / 1000.0f;

        switch (mode) {
        case kModeSolid: {
            strip.fill(scale8(cr, bright), scale8(cg, bright), scale8(cb, bright));
            break;
        }
        case kModeBreath: {
            // 4 s period sine, biased so dim doesn't fully extinguish (32/255
            // floor keeps the strip visibly "on" at the trough).
            const float phase = std::sin(t * 2.0f * 3.14159265f / 4.0f);
            const float gain = (phase * 0.5f + 0.5f) * 0.85f + 0.15f;
            const std::uint8_t b2 = static_cast<std::uint8_t>(bright * gain);
            strip.fill(scale8(cr, b2), scale8(cg, b2), scale8(cb, b2));
            break;
        }
        case kModeGradient: {
            // Full-strip rainbow that scrolls one full revolution every 6 s.
            // The colour stored in led_color is ignored in this mode (the hue
            // is generated) — only brightness applies.
            const float h0 = t / 6.0f;
            for (std::size_t i = 0; i < n; ++i) {
                std::uint8_t r, g, b;
                hsv_to_rgb(h0 + static_cast<float>(i) / static_cast<float>(n), r, g, b);
                strip.set(i, scale8(r, bright), scale8(g, bright), scale8(b, bright));
            }
            break;
        }
        case kModeOff:
        default:
            strip.clear();
            break;
        }

        (void)strip.show();
        vTaskDelayUntil(&last_wake, kPeriodTicks);
    }
}

} // namespace

void start_led_task(LedTaskArgs& args)
{
    // 4 KiB is comfortable for the sin/HSV math + 64 B local frame buffer.
    // Core 1 keeps the I2C bursts off core 0 where NimBLE + Wi-Fi live.
    xTaskCreatePinnedToCore(led_task_entry, "led", 4096, &args, 2, nullptr, 1);
}

} // namespace stackchan::app
