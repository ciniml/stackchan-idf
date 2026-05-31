// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <string_view>

namespace stackchan::app {

// Per-servo zero-position (raw SCS0009 step, 0..1023) and the soft motion range
// in degrees relative to that zero. The Takao base mounts the head differently
// from the M5 base, so these are configurable + persisted to NVS. Defaults
// match the historical M5 base values (kYawZero=460, kPitchZero=620, yaw ±40°,
// pitch -10..+25°), so an empty/missing config reproduces the original head.
struct ServoLimits {
    std::uint16_t yaw_zero = 460;
    int yaw_min_deg = -40;
    int yaw_max_deg = 40;
    std::uint16_t pitch_zero = 620;
    int pitch_min_deg = -10;
    int pitch_max_deg = 25;
};

// Parse the compact JSON the BLE / Wi-Fi settings UI sends into a ServoLimits.
// Missing keys keep their defaults; empty / malformed input yields all defaults.
// Schema (all optional):
//   {"yaw_zero":<u10>,"yaw_min":<int>,"yaw_max":<int>,
//    "pitch_zero":<u10>,"pitch_min":<int>,"pitch_max":<int>}
ServoLimits parse_servo_limits(std::string_view json);

// Clamp a degree value to [min, max], degree-typed.
float clamp_deg(float deg, int lo, int hi) noexcept;

} // namespace stackchan::app
