// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <string_view>

#include "avatar/face_tuning.hpp"

namespace stackchan::app {

// Parse the compact face-config JSON written by the settings UI into a
// FaceTuning. Missing / malformed keys keep their FaceTuning defaults, so an
// empty or invalid string yields the built-in default face. Shared by the
// boot-time load and the live BLE update path.
//
// Schema (all keys optional):
//   {"brow":0|1, "eye_r":<num>, "eye_ox":<num>, "eye_oy":<num>,
//    "brow_ox":<num>, "brow_oy":<num>, "mouth_ox":<num>, "mouth_oy":<num>,
//    "mouth_minw":<int>, "mouth_maxw":<int>, "mouth_minh":<int>, "mouth_maxh":<int>,
//    "face_color":"#rrggbb", "bg_color":"#rrggbb"}
avatar::FaceTuning parse_face_tuning(std::string_view json);

} // namespace stackchan::app
