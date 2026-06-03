// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>

// Avatar DSL bytecode opcodes + context variable IDs. This header is the
// canonical source for the C++ VM. The JS compiler ships an independent
// transcription at tools/avatar_dsl/opcodes.js — keep the two in sync (the
// bytecode header magic + version pin guards against silent drift).

namespace stackchan::avatar_vm {

inline constexpr std::uint32_t kMagic = 0x53445641u; // "AVDS" little-endian
inline constexpr std::uint16_t kVersion = 1;

enum class Op : std::uint8_t {
    Nop = 0x00,

    PushF32 = 0x01,    // <4B LE f32>
    PushI8 = 0x02,     // <1B signed>
    PushI16 = 0x03,    // <2B LE signed>
    PushConst = 0x04,  // <1B id>
    PushVar = 0x05,    // <1B id>
    PushLocal = 0x06,  // <1B slot>
    StoreLocal = 0x07, // <1B slot>
    Pop = 0x08,
    Dup = 0x09,

    Add = 0x10,
    Sub = 0x11,
    Mul = 0x12,
    Div = 0x13,
    Neg = 0x14,
    Min = 0x15,
    Max = 0x16,
    Abs = 0x17,
    Floor = 0x18,
    Round = 0x19,
    Mod = 0x1A,
    Sqrt = 0x1B,
    Clamp = 0x1C,
    Scale = 0x1D,
    Tx = 0x1E,
    Ty = 0x1F,

    Eq = 0x20,
    Ne = 0x21,
    Lt = 0x22,
    Le = 0x23,
    Gt = 0x24,
    Ge = 0x25,
    Not = 0x26,
    And = 0x27,
    Or = 0x28,
    Xor = 0x29,

    Jmp = 0x30, // <2B LE i16>
    Jz = 0x31,  // <2B LE i16>
    Jnz = 0x32, // <2B LE i16>
    Call = 0x33, // <1B fn_id>
    Ret = 0x34,

    FillRect = 0x40,
    FillCircle = 0x41,
    FillTriangle = 0x42,
    // 0x43/0x44 reserved for fillRoundRect/drawRoundRect
    BeginGroup = 0x45,
    EndGroup = 0x46,
};

// Context variable IDs (read-only, pushed by PushVar).
enum class Var : std::uint8_t {
    CanvasW = 0x00,
    CanvasH = 0x01,
    CanvasScale = 0x02,
    NowMs = 0x03,
    Breath = 0x04,
    EyeOpen = 0x05,
    GazeH = 0x06,
    GazeV = 0x07,
    MouthOpen = 0x08,
    Expr = 0x09,
    Primary = 0x0A,
    Background = 0x0B,
    Secondary = 0x0C,
    BalloonFg = 0x0D,
    BalloonBg = 0x0E,
    EyeRadius = 0x0F,
    EyeOffX = 0x10,
    EyeOffY = 0x11,
    BrowOffX = 0x12,
    BrowOffY = 0x13,
    MouthOffX = 0x14,
    MouthOffY = 0x15,
    MouthMinW = 0x16,
    MouthMaxW = 0x17,
    MouthMinH = 0x18,
    MouthMaxH = 0x19,
    EyebrowsVisible = 0x1A,
    VarCount,
};

// Const table tag bytes.
enum class ConstTag : std::uint8_t {
    F32 = 0x01,    // 4B f32 LE
    I32 = 0x02,    // 4B i32 LE
    Color = 0x03,  // 2B u16 LE (RGB565)
    String = 0x04, // (reserved) 2B u16 len + bytes
};

// Expression enum values (mirror stackchan::avatar::Expression).
enum class ExprValue : std::uint8_t {
    Neutral = 0,
    Happy = 1,
    Sad = 2,
    Angry = 3,
    Doubt = 4,
    Sleepy = 5,
};

} // namespace stackchan::avatar_vm
