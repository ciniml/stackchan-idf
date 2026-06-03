// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar_vm/default_bytecode.hpp"

// ESP-IDF emits these symbols when target_add_binary_data(... BINARY) is
// called on default_face.avbc. The asm() name strips any path mangling so it
// matches across host vs. cross-compile linkers.
extern "C" {
extern const std::uint8_t _binary_default_face_avbc_start[] asm("_binary_default_face_avbc_start");
extern const std::uint8_t _binary_default_face_avbc_end[] asm("_binary_default_face_avbc_end");
}

namespace stackchan::avatar_vm {

std::span<const std::uint8_t> default_face_bytecode() noexcept
{
    return std::span<const std::uint8_t>(
        _binary_default_face_avbc_start,
        static_cast<std::size_t>(_binary_default_face_avbc_end - _binary_default_face_avbc_start));
}

} // namespace stackchan::avatar_vm
