// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstdint>

#include <tl/expected.hpp>

#include "avatar/canvas.hpp"
#include "avatar/draw_context.hpp"
#include "avatar/face_tuning.hpp"
#include "avatar_vm/bytecode.hpp"

namespace stackchan::avatar_vm {

// Stack-based VM that executes one frame of avatar drawing per `run()` call.
// The avatar's animator (in components/avatar) updates DrawContext before each
// call; the VM treats both `ctx` and `tuning` as read-only inputs.
class Vm {
public:
    static constexpr std::size_t kStackSize = 64;
    static constexpr std::size_t kMaxLocals = 256;
    static constexpr std::size_t kMaxCallDepth = 16;

    Vm() = default;

    // Run the bytecode's entry function. Returns success/failure; the canvas
    // has been drawn into either way (partial frames are possible on error).
    tl::expected<void, VmError> run(const Bytecode& bc, avatar::Canvas& canvas,
                                    const avatar::DrawContext& ctx,
                                    const avatar::FaceTuning& tuning);

private:
    struct Frame {
        std::uint16_t return_pc; // PC to resume at after RET (offset into code section)
        std::uint16_t locals_base; // index into locals_ where this frame's locals start
        std::uint16_t locals_size; // total locals (including params)
    };

    std::array<float, kStackSize> stack_{};
    std::size_t sp_ = 0; // points to next free slot; stack_[sp_ - 1] is top

    std::array<float, kMaxLocals> locals_{};

    std::array<Frame, kMaxCallDepth> frames_{};
    std::size_t fp_ = 0;
};

} // namespace stackchan::avatar_vm
