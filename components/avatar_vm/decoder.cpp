// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar_vm/bytecode.hpp"

#include <cstring>

namespace stackchan::avatar_vm {

namespace {

constexpr std::size_t kHeaderSize = 16;

inline std::uint16_t rd_u16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

inline std::int16_t rd_i16(const std::uint8_t* p) noexcept { return static_cast<std::int16_t>(rd_u16(p)); }

inline std::uint32_t rd_u32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::int32_t rd_i32(const std::uint8_t* p) noexcept { return static_cast<std::int32_t>(rd_u32(p)); }

inline float rd_f32(const std::uint8_t* p) noexcept
{
    float f;
    std::uint32_t u = rd_u32(p);
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

} // namespace

const char* to_string(VmError e) noexcept
{
    switch (e) {
    case VmError::BadMagic: return "bad magic";
    case VmError::BadVersion: return "bad version";
    case VmError::Truncated: return "truncated";
    case VmError::BadConstTag: return "bad const tag";
    case VmError::UnknownOpcode: return "unknown opcode";
    case VmError::BadVarId: return "bad var id";
    case VmError::BadConstId: return "bad const id";
    case VmError::BadFnId: return "bad fn id";
    case VmError::BadLocalSlot: return "bad local slot";
    case VmError::StackUnderflow: return "stack underflow";
    case VmError::StackOverflow: return "stack overflow";
    case VmError::CallDepthExceeded: return "call depth exceeded";
    case VmError::JumpOutOfBounds: return "jump out of bounds";
    case VmError::DivideByZero: return "divide by zero";
    case VmError::EntryFnInvalid: return "entry fn invalid";
    }
    return "?";
}

tl::expected<Bytecode, VmError> decode(std::span<const std::uint8_t> data)
{
    if (data.size() < kHeaderSize) {
        return tl::unexpected(VmError::Truncated);
    }
    const std::uint8_t* p = data.data();
    if (rd_u32(p) != kMagic) {
        return tl::unexpected(VmError::BadMagic);
    }
    if (rd_u16(p + 4) != kVersion) {
        return tl::unexpected(VmError::BadVersion);
    }
    const std::uint16_t const_count = rd_u16(p + 8);
    const std::uint16_t fn_count = rd_u16(p + 10);
    const std::uint16_t code_size = rd_u16(p + 12);
    const std::uint16_t entry_fn = rd_u16(p + 14);

    std::size_t off = kHeaderSize;
    Bytecode bc;
    bc.entry_fn_id = entry_fn;
    bc.consts.reserve(const_count);

    for (std::uint16_t i = 0; i < const_count; ++i) {
        if (off + 1 > data.size()) return tl::unexpected(VmError::Truncated);
        const std::uint8_t tag = data[off++];
        switch (static_cast<ConstTag>(tag)) {
        case ConstTag::F32:
            if (off + 4 > data.size()) return tl::unexpected(VmError::Truncated);
            bc.consts.push_back(rd_f32(data.data() + off));
            off += 4;
            break;
        case ConstTag::I32:
            if (off + 4 > data.size()) return tl::unexpected(VmError::Truncated);
            bc.consts.push_back(static_cast<float>(rd_i32(data.data() + off)));
            off += 4;
            break;
        case ConstTag::Color:
            if (off + 2 > data.size()) return tl::unexpected(VmError::Truncated);
            bc.consts.push_back(static_cast<float>(rd_u16(data.data() + off)));
            off += 2;
            break;
        default:
            return tl::unexpected(VmError::BadConstTag);
        }
    }

    bc.fns.reserve(fn_count);
    for (std::uint16_t i = 0; i < fn_count; ++i) {
        if (off + 6 > data.size()) return tl::unexpected(VmError::Truncated);
        FnEntry fe;
        fe.code_offset = rd_u16(data.data() + off);
        fe.param_count = data[off + 2];
        fe.local_count = data[off + 3];
        // reserved (2 bytes) ignored
        if (fe.local_count < fe.param_count) {
            return tl::unexpected(VmError::BadLocalSlot);
        }
        bc.fns.push_back(fe);
        off += 6;
    }

    if (off + code_size > data.size()) {
        return tl::unexpected(VmError::Truncated);
    }
    bc.code = data.subspan(off, code_size);

    if (entry_fn >= bc.fns.size()) {
        return tl::unexpected(VmError::EntryFnInvalid);
    }
    if (bc.fns[entry_fn].code_offset >= code_size) {
        return tl::unexpected(VmError::EntryFnInvalid);
    }
    if (bc.fns[entry_fn].param_count != 0) {
        return tl::unexpected(VmError::EntryFnInvalid);
    }
    return bc;
}

} // namespace stackchan::avatar_vm
