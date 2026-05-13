// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "face.hpp"

namespace stackchan::avatar::internal {

void draw_face(M5Canvas& canvas, const Face& face, const DrawContext& ctx)
{
    const std::int16_t breath_offset_y = static_cast<std::int16_t>(ctx.breath * 3.0f);
    draw_mouth(canvas, face.mouth, ctx, breath_offset_y);
    draw_eye(canvas, face.eye_left, ctx, breath_offset_y);
    draw_eye(canvas, face.eye_right, ctx, breath_offset_y);
    draw_eyebrow(canvas, face.eyebrow_left, ctx, breath_offset_y);
    draw_eyebrow(canvas, face.eyebrow_right, ctx, breath_offset_y);
}

} // namespace stackchan::avatar::internal
