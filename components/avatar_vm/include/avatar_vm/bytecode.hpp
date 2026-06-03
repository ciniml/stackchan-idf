// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <tl/expected.hpp>

#include "avatar_vm/opcodes.hpp"

namespace stackchan::avatar_vm {

// One entry in the function table. Code offset is into the bytecode's code
// section (not the file). Locals include the parameters at slots [0..param_count).
struct FnEntry {
    std::uint16_t code_offset;
    std::uint8_t param_count;
    std::uint8_t local_count;
};

// Decoded bytecode held in RAM. The code section is borrowed (zero-copy) from
// the source buffer when possible; const/fn tables are copied into vectors
// because they're tiny and decoding sorts out the variable-length entries.
struct Bytecode {
    // Numeric constants. Booleans / colours are promoted to f32 here so the VM
    // only ever sees floats on the operand stack.
    std::vector<float> consts;
    std::vector<FnEntry> fns;
    // Borrowed view into the original buffer (must outlive Bytecode).
    std::span<const std::uint8_t> code;
    std::uint16_t entry_fn_id;
};

enum class VmError : std::uint8_t {
    BadMagic = 1,
    BadVersion,
    Truncated,        // header / table / code shorter than declared
    BadConstTag,
    UnknownOpcode,
    BadVarId,
    BadConstId,
    BadFnId,
    BadLocalSlot,
    StackUnderflow,
    StackOverflow,
    CallDepthExceeded,
    JumpOutOfBounds,
    DivideByZero,
    EntryFnInvalid,
};

const char* to_string(VmError e) noexcept;

// Parse the on-disk / on-wire bytecode buffer. `data` must outlive the returned
// Bytecode (the code section is a span into it).
tl::expected<Bytecode, VmError> decode(std::span<const std::uint8_t> data);

} // namespace stackchan::avatar_vm
