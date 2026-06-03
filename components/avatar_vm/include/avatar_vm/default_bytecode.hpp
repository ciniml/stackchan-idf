// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <span>

namespace stackchan::avatar_vm {

// Bytes of the build-time-compiled default face bytecode (see
// assets/default_face.avdsl + CMakeLists.txt). Always available; firmware can
// fall back to this whenever NVS holds no override or the override fails to
// decode.
std::span<const std::uint8_t> default_face_bytecode() noexcept;

} // namespace stackchan::avatar_vm
