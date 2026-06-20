// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>

namespace stackchan::avatar {

// User-adjustable face layout + colours. Defaults mirror the standard
// Stack-chan layout (internal::Face's member initialisers) so a default-
// constructed FaceTuning reproduces the built-in face exactly.
//
// Positions are authored for the 320x240 design canvas; internal::build_face()
// scales + centres them to the actual render canvas. Horizontal offsets are
// applied symmetrically (positive eye_off_x / brow_off_x spreads the pair
// apart). The two colours map onto Palette::primary (face) and
// Palette::background; the remaining palette entries keep their defaults.
struct FaceTuning {
    bool eyebrows_visible = true;
    float eye_radius = 8.0f;
    float eye_off_x = 0.0f, eye_off_y = 0.0f;
    float brow_off_x = 0.0f, brow_off_y = 0.0f;
    float mouth_off_x = 0.0f, mouth_off_y = 0.0f;
    int mouth_min_w = 50, mouth_max_w = 90; // resting / open width range
    int mouth_min_h = 4, mouth_max_h = 60;  // closed / open height range
    // Cheek mark (optional, off by default to keep the standard face
    // unchanged for existing users). Same horizontal-symmetric layout as
    // eyes / brows: positive cheek_off_x spreads the pair outward.
    // Radius is in 320x240 design pixels; the renderer scales it to the
    // actual canvas alongside the eye positions.
    bool cheeks_visible = false;
    float cheek_radius = 10.0f;
    float cheek_off_x = 0.0f, cheek_off_y = 0.0f;
    std::uint16_t face_color = 0xFFFFu;      // RGB565 → Palette::primary
    std::uint16_t bg_color = 0x0000u;        // RGB565 → Palette::background
};

} // namespace stackchan::avatar
