// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "face_config.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

#include <cJSON.h>

namespace stackchan::app {

namespace {

// "#rrggbb" (or "rrggbb") → RGB565. Returns `fallback` on a malformed string.
std::uint16_t hex_to_565(const char* s, std::uint16_t fallback)
{
    if (s == nullptr) return fallback;
    if (*s == '#') ++s;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 16);
    if (end == s || v < 0 || v > 0xFFFFFF) return fallback;
    const int r = (static_cast<int>(v) >> 16) & 0xFF;
    const int g = (static_cast<int>(v) >> 8) & 0xFF;
    const int b = static_cast<int>(v) & 0xFF;
    return static_cast<std::uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void apply_number(const cJSON* root, const char* key, float& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) out = static_cast<float>(item->valuedouble);
}

void apply_int(const cJSON* root, const char* key, int& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) out = item->valueint;
}

} // namespace

avatar::FaceTuning parse_face_tuning(std::string_view json)
{
    avatar::FaceTuning t; // start from defaults
    if (json.empty()) return t;

    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (root == nullptr) return t;

    const cJSON* brow = cJSON_GetObjectItemCaseSensitive(root, "brow");
    if (cJSON_IsBool(brow)) {
        t.eyebrows_visible = cJSON_IsTrue(brow);
    } else if (cJSON_IsNumber(brow)) {
        t.eyebrows_visible = brow->valueint != 0;
    }

    apply_number(root, "eye_r", t.eye_radius);
    apply_number(root, "eye_ox", t.eye_off_x);
    apply_number(root, "eye_oy", t.eye_off_y);
    apply_number(root, "brow_ox", t.brow_off_x);
    apply_number(root, "brow_oy", t.brow_off_y);
    apply_number(root, "mouth_ox", t.mouth_off_x);
    apply_number(root, "mouth_oy", t.mouth_off_y);
    apply_int(root, "mouth_minw", t.mouth_min_w);
    apply_int(root, "mouth_maxw", t.mouth_max_w);
    apply_int(root, "mouth_minh", t.mouth_min_h);
    apply_int(root, "mouth_maxh", t.mouth_max_h);

    const cJSON* face_color = cJSON_GetObjectItemCaseSensitive(root, "face_color");
    if (cJSON_IsString(face_color)) t.face_color = hex_to_565(face_color->valuestring, t.face_color);
    const cJSON* bg_color = cJSON_GetObjectItemCaseSensitive(root, "bg_color");
    if (cJSON_IsString(bg_color)) t.bg_color = hex_to_565(bg_color->valuestring, t.bg_color);

    cJSON_Delete(root);
    return t;
}

} // namespace stackchan::app
