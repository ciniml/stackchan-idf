// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5GFX.h>

#include "avatar/draw_context.hpp"
#include "avatar/face_tuning.hpp"
#include "eye.hpp"
#include "eyebrow.hpp"
#include "mouth.hpp"

namespace stackchan::avatar::internal {

// Standard Stack-chan face layout for a 320x240 canvas, mirroring
// m5stack-avatar-rs::Face::default. "left" / "right" follow the avatar's
// own anatomy, so eye_left is at the viewer's right.
struct Face {
    // is_left follows m5stack-avatar-rs: the eye/eyebrow at the viewer's RIGHT
    // (x≈230) carry is_left=false, those at the viewer's LEFT (x≈90) carry
    // is_left=true. (The earlier flipped flags mirrored the Angry/Sad tilts.)
    Eye eye_left{230, 96, 8.0f, false};
    Eye eye_right{90, 93, 8.0f, true};
    Mouth mouth{163, 148, 50, 90, 4, 60};
    Eyebrow eyebrow_left{96, 67, 32, 2, false};
    Eyebrow eyebrow_right{230, 72, 32, 2, true};
    // Display toggle: when false the eyebrows are not drawn.
    bool show_eyebrows{true};
};

void draw_face(M5Canvas& canvas, const Face& face, const DrawContext& ctx);

// Build a Face from user tuning, scaled + centred onto a canvas_w x canvas_h
// target. The base layout is authored for 320x240; a uniform scale (preserving
// aspect) maps it to the canvas. Shared by the firmware (Avatar::set_face_tuning,
// 320x240) and the WASM preview (any size) so both render identically.
Face build_face(const FaceTuning& tuning, std::int16_t canvas_w, std::int16_t canvas_h);

} // namespace stackchan::avatar::internal
