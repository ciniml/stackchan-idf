// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "servo_limits.hpp"

#include <algorithm>

#include <cJSON.h>

namespace stackchan::app {

namespace {

void apply_u16(const cJSON* root, const char* key, std::uint16_t& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        const int v = item->valueint;
        if (v >= 0 && v <= 1023) out = static_cast<std::uint16_t>(v);
    }
}

void apply_int(const cJSON* root, const char* key, int& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) out = item->valueint;
}

} // namespace

ServoLimits parse_servo_limits(std::string_view json)
{
    ServoLimits l;
    if (json.empty()) return l;
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (root == nullptr) return l;
    apply_u16(root, "yaw_zero", l.yaw_zero);
    apply_int(root, "yaw_min", l.yaw_min_deg);
    apply_int(root, "yaw_max", l.yaw_max_deg);
    apply_u16(root, "pitch_zero", l.pitch_zero);
    apply_int(root, "pitch_min", l.pitch_min_deg);
    apply_int(root, "pitch_max", l.pitch_max_deg);
    // Defensive: keep min ≤ max.
    if (l.yaw_min_deg > l.yaw_max_deg) std::swap(l.yaw_min_deg, l.yaw_max_deg);
    if (l.pitch_min_deg > l.pitch_max_deg) std::swap(l.pitch_min_deg, l.pitch_max_deg);
    cJSON_Delete(root);
    return l;
}

float clamp_deg(float deg, int lo, int hi) noexcept
{
    const float flo = static_cast<float>(lo);
    const float fhi = static_cast<float>(hi);
    if (deg < flo) return flo;
    if (deg > fhi) return fhi;
    return deg;
}

} // namespace stackchan::app
