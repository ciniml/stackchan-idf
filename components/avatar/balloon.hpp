// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5GFX.h>

#include "avatar/draw_context.hpp"

namespace stackchan::avatar::internal {

void draw_balloon(M5Canvas& canvas, DrawContext& ctx);

} // namespace stackchan::avatar::internal
