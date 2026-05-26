// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "face.hpp"

#include <algorithm>
#include <cstdint>

namespace stackchan::avatar::internal {

namespace {
// Design resolution the base layout (internal::Face's member initialisers) is
// authored for.
constexpr float kBaseW = 320.0f;
constexpr float kBaseH = 240.0f;
constexpr float kBaseCx = 160.0f;
constexpr float kBaseCy = 120.0f;
} // namespace

Face build_face(const FaceTuning& tuning, std::int16_t canvas_w, std::int16_t canvas_h)
{
    // Uniform scale (preserve aspect) of the base layout to the canvas, centred.
    const float scale = std::min(static_cast<float>(canvas_w) / kBaseW,
                                 static_cast<float>(canvas_h) / kBaseH);
    const float cx = canvas_w / 2.0f;
    const float cy = canvas_h / 2.0f;
    auto tx = [&](float bx) { return static_cast<std::int16_t>(cx + (bx - kBaseCx) * scale); };
    auto ty = [&](float by) { return static_cast<std::int16_t>(cy + (by - kBaseCy) * scale); };
    auto sz = [&](float base) {
        const float v = base * scale;
        return static_cast<std::uint16_t>(v < 1.0f ? 1.0f : v);
    };
    const float radius = tuning.eye_radius * scale < 1.0f ? 1.0f : tuning.eye_radius * scale;
    const int max_w = tuning.mouth_max_w < tuning.mouth_min_w ? tuning.mouth_min_w : tuning.mouth_max_w;
    const int max_h = tuning.mouth_max_h < tuning.mouth_min_h ? tuning.mouth_min_h : tuning.mouth_max_h;

    Face face;
    face.eye_left = Eye{tx(230 + tuning.eye_off_x), ty(96 + tuning.eye_off_y), radius, false};
    face.eye_right = Eye{tx(90 - tuning.eye_off_x), ty(93 + tuning.eye_off_y), radius, true};
    face.eyebrow_left = Eyebrow{tx(96 - tuning.brow_off_x), ty(67 + tuning.brow_off_y), sz(32), sz(2), false};
    face.eyebrow_right = Eyebrow{tx(230 + tuning.brow_off_x), ty(72 + tuning.brow_off_y), sz(32), sz(2), true};
    face.mouth = Mouth{tx(163 + tuning.mouth_off_x), ty(148 + tuning.mouth_off_y),
                       sz(static_cast<float>(tuning.mouth_min_w)), sz(static_cast<float>(max_w)),
                       sz(static_cast<float>(tuning.mouth_min_h)), sz(static_cast<float>(max_h))};
    face.show_eyebrows = tuning.eyebrows_visible;
    return face;
}

void draw_face(M5Canvas& canvas, const Face& face, const DrawContext& ctx)
{
    const std::int16_t breath_offset_y = static_cast<std::int16_t>(ctx.breath * 3.0f);
    draw_mouth(canvas, face.mouth, ctx, breath_offset_y);
    draw_eye(canvas, face.eye_left, ctx, breath_offset_y);
    draw_eye(canvas, face.eye_right, ctx, breath_offset_y);
    if (face.show_eyebrows) {
        draw_eyebrow(canvas, face.eyebrow_left, ctx, breath_offset_y);
        draw_eyebrow(canvas, face.eyebrow_right, ctx, breath_offset_y);
    }
}

} // namespace stackchan::avatar::internal
