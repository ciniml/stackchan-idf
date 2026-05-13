// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5GFX.h>

#include "avatar/draw_context.hpp"
#include "eye.hpp"
#include "eyebrow.hpp"
#include "mouth.hpp"

namespace stackchan::avatar::internal {

// Standard Stack-chan face layout for a 320x240 canvas, mirroring
// m5stack-avatar-rs::Face::default. "left" / "right" follow the avatar's
// own anatomy, so eye_left is at the viewer's right.
struct Face {
    Eye eye_left{230, 96, 8.0f, true};
    Eye eye_right{90, 93, 8.0f, false};
    Mouth mouth{163, 148, 50, 90, 4, 60};
    Eyebrow eyebrow_left{96, 67, 32, 2, true};
    Eyebrow eyebrow_right{230, 72, 32, 2, false};
};

void draw_face(M5Canvas& canvas, const Face& face, const DrawContext& ctx);

} // namespace stackchan::avatar::internal
