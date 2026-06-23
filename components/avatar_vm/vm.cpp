// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar_vm/vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace stackchan::avatar_vm {

namespace {

inline std::uint16_t rd_u16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}
inline std::int16_t rd_i16(const std::uint8_t* p) noexcept { return static_cast<std::int16_t>(rd_u16(p)); }
inline float rd_f32(const std::uint8_t* p) noexcept
{
    float f;
    std::uint32_t u = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                      (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline std::int16_t f2i(float v) noexcept { return static_cast<std::int16_t>(v); }
inline std::uint16_t f2color(float v) noexcept { return static_cast<std::uint16_t>(v); }

inline float expression_to_f(avatar::Expression e) noexcept
{
    return static_cast<float>(static_cast<std::uint8_t>(e));
}

// Compute the design→canvas uniform scale the original face.cpp used.
// For circular displays we fit the 320×240 design rect by its DIAGONAL
// to the panel diameter, with a small margin so corner-anchored effect
// marks (heart / sweat / spark on the 320×240 design's top-right) don't
// graze the visible circle's edge. The earlier 1/√2 inset matched a
// SQUARE design (320×320) and so under-scaled the actual 4:3 rect by
// ~10% on 466×466 — using the proper rectangle diagonal recovers that.
inline float canvas_scale(const avatar::Canvas& c) noexcept
{
    constexpr float kBaseW = 320.0f;
    constexpr float kBaseH = 240.0f;
    if (c.is_circular()) {
        // Round AMOLEDs report width == height == diameter, so either dim
        // gives the diameter. Slight margin (0.97) keeps the very corner
        // pixels just inside the visible edge.
        constexpr float kCornerMargin = 0.97f;
        constexpr float kBaseDiag = 400.0f;  // sqrt(320² + 240²)
        const float diameter = static_cast<float>(std::min(c.width(), c.height()));
        return diameter / kBaseDiag * kCornerMargin;
    }
    return std::min(static_cast<float>(c.width()) / kBaseW,
                    static_cast<float>(c.height()) / kBaseH);
}

inline float read_var(Var v, const avatar::Canvas& canvas, const avatar::DrawContext& ctx,
                      const avatar::FaceTuning& t, float scale)
{
    switch (v) {
    case Var::CanvasW: return static_cast<float>(canvas.width());
    case Var::CanvasH: return static_cast<float>(canvas.height());
    case Var::CanvasScale: return scale;
    case Var::NowMs: return static_cast<float>(ctx.now_ms);
    case Var::Breath: return ctx.breath;
    case Var::EyeOpen: return ctx.eye_open_ratio;
    // gaze_h / gaze_v in the DSL is the sum of the externally-commanded
    // target (set via Avatar::set_gaze, e.g. StopWatch touch-follow) and
    // the animator's saccade offset. This lets the eyes wander around a
    // commanded target rather than locking dead-stop on it.
    case Var::GazeH: return ctx.gaze_horizontal + ctx.gaze_saccade_h;
    case Var::GazeV: return ctx.gaze_vertical + ctx.gaze_saccade_v;
    case Var::MouthOpen: return ctx.mouth_open_ratio;
    case Var::Expr: return expression_to_f(ctx.expression);
    case Var::Primary: return static_cast<float>(ctx.palette.primary);
    case Var::Background: return static_cast<float>(ctx.palette.background);
    case Var::Secondary: return static_cast<float>(ctx.palette.secondary);
    case Var::BalloonFg: return static_cast<float>(ctx.palette.balloon_foreground);
    case Var::BalloonBg: return static_cast<float>(ctx.palette.balloon_background);
    case Var::EyeRadius: return t.eye_radius;
    case Var::EyeOffX: return t.eye_off_x;
    case Var::EyeOffY: return t.eye_off_y;
    case Var::BrowOffX: return t.brow_off_x;
    case Var::BrowOffY: return t.brow_off_y;
    case Var::MouthOffX: return t.mouth_off_x;
    case Var::MouthOffY: return t.mouth_off_y;
    case Var::MouthMinW: return static_cast<float>(t.mouth_min_w);
    case Var::MouthMaxW: return static_cast<float>(t.mouth_max_w);
    case Var::MouthMinH: return static_cast<float>(t.mouth_min_h);
    case Var::MouthMaxH: return static_cast<float>(t.mouth_max_h);
    case Var::EyebrowsVisible: return t.eyebrows_visible ? 1.0f : 0.0f;
    case Var::CheeksVisible: return t.cheeks_visible ? 1.0f : 0.0f;
    case Var::CheekRadius: return t.cheek_radius;
    case Var::CheekOffX: return t.cheek_off_x;
    case Var::CheekOffY: return t.cheek_off_y;
    default: return 0.0f;
    }
}

} // namespace

tl::expected<void, VmError> Vm::run(const Bytecode& bc, avatar::Canvas& canvas,
                                    const avatar::DrawContext& ctx, const avatar::FaceTuning& tuning)
{
    sp_ = 0;
    fp_ = 0;
    const float scale = canvas_scale(canvas);
    const float cx_screen = canvas.width() / 2.0f;
    const float cy_screen = canvas.height() / 2.0f;

    auto push = [&](float v) -> tl::expected<void, VmError> {
        if (sp_ >= kStackSize) return tl::unexpected(VmError::StackOverflow);
        stack_[sp_++] = v;
        return {};
    };
    auto pop = [&](float& out) -> tl::expected<void, VmError> {
        if (sp_ == 0) return tl::unexpected(VmError::StackUnderflow);
        out = stack_[--sp_];
        return {};
    };

    // Set up entry frame. Entry fn has 0 params; allocate its locals.
    // (local_count is uint8_t and kMaxLocals == 256, so a runtime check would
    // be a no-op; the decoder guarantees the value fits.)
    const FnEntry& entry = bc.fns[bc.entry_fn_id];
    Frame& root = frames_[fp_++];
    root.return_pc = 0; // sentinel: program ends when this frame returns
    root.locals_base = 0;
    root.locals_size = entry.local_count;
    for (std::size_t i = 0; i < entry.local_count; ++i) locals_[i] = 0.0f;

    const std::uint8_t* code = bc.code.data();
    const std::size_t code_size = bc.code.size();
    std::size_t pc = entry.code_offset;

    auto jump = [&](std::int16_t off, std::size_t pc_after_operand) -> tl::expected<void, VmError> {
        const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(pc_after_operand) + off;
        if (target < 0 || target > static_cast<std::ptrdiff_t>(code_size)) {
            return tl::unexpected(VmError::JumpOutOfBounds);
        }
        pc = static_cast<std::size_t>(target);
        return {};
    };

    while (true) {
        if (pc >= code_size) return tl::unexpected(VmError::Truncated);
        const std::uint8_t op = code[pc++];
        switch (static_cast<Op>(op)) {
        case Op::Nop: break;

        case Op::PushF32: {
            if (pc + 4 > code_size) return tl::unexpected(VmError::Truncated);
            auto r = push(rd_f32(code + pc));
            if (!r) return r;
            pc += 4;
            break;
        }
        case Op::PushI8: {
            if (pc + 1 > code_size) return tl::unexpected(VmError::Truncated);
            auto r = push(static_cast<float>(static_cast<std::int8_t>(code[pc])));
            if (!r) return r;
            pc += 1;
            break;
        }
        case Op::PushI16: {
            if (pc + 2 > code_size) return tl::unexpected(VmError::Truncated);
            auto r = push(static_cast<float>(rd_i16(code + pc)));
            if (!r) return r;
            pc += 2;
            break;
        }
        case Op::PushConst: {
            if (pc + 1 > code_size) return tl::unexpected(VmError::Truncated);
            const std::uint8_t id = code[pc++];
            if (id >= bc.consts.size()) return tl::unexpected(VmError::BadConstId);
            auto r = push(bc.consts[id]);
            if (!r) return r;
            break;
        }
        case Op::PushVar: {
            if (pc + 1 > code_size) return tl::unexpected(VmError::Truncated);
            const std::uint8_t id = code[pc++];
            if (id >= static_cast<std::uint8_t>(Var::VarCount)) return tl::unexpected(VmError::BadVarId);
            auto r = push(read_var(static_cast<Var>(id), canvas, ctx, tuning, scale));
            if (!r) return r;
            break;
        }
        case Op::PushLocal: {
            if (pc + 1 > code_size) return tl::unexpected(VmError::Truncated);
            const std::uint8_t slot = code[pc++];
            Frame& f = frames_[fp_ - 1];
            if (slot >= f.locals_size) return tl::unexpected(VmError::BadLocalSlot);
            auto r = push(locals_[f.locals_base + slot]);
            if (!r) return r;
            break;
        }
        case Op::StoreLocal: {
            if (pc + 1 > code_size) return tl::unexpected(VmError::Truncated);
            const std::uint8_t slot = code[pc++];
            Frame& f = frames_[fp_ - 1];
            if (slot >= f.locals_size) return tl::unexpected(VmError::BadLocalSlot);
            float v;
            if (auto r = pop(v); !r) return r;
            locals_[f.locals_base + slot] = v;
            break;
        }
        case Op::Pop: {
            float v;
            if (auto r = pop(v); !r) return r;
            break;
        }
        case Op::Dup: {
            if (sp_ == 0) return tl::unexpected(VmError::StackUnderflow);
            if (sp_ >= kStackSize) return tl::unexpected(VmError::StackOverflow);
            stack_[sp_] = stack_[sp_ - 1];
            ++sp_;
            break;
        }

        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
        case Op::Min: case Op::Max: case Op::Mod:
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        case Op::And: case Op::Or: case Op::Xor: {
            float b, a;
            if (auto r = pop(b); !r) return r;
            if (auto r = pop(a); !r) return r;
            float v = 0.0f;
            switch (static_cast<Op>(op)) {
            case Op::Add: v = a + b; break;
            case Op::Sub: v = a - b; break;
            case Op::Mul: v = a * b; break;
            case Op::Div:
                if (b == 0.0f) return tl::unexpected(VmError::DivideByZero);
                v = a / b;
                break;
            case Op::Min: v = std::min(a, b); break;
            case Op::Max: v = std::max(a, b); break;
            case Op::Mod:
                if (b == 0.0f) return tl::unexpected(VmError::DivideByZero);
                v = std::fmod(a, b);
                break;
            case Op::Eq: v = (a == b) ? 1.0f : 0.0f; break;
            case Op::Ne: v = (a != b) ? 1.0f : 0.0f; break;
            case Op::Lt: v = (a < b) ? 1.0f : 0.0f; break;
            case Op::Le: v = (a <= b) ? 1.0f : 0.0f; break;
            case Op::Gt: v = (a > b) ? 1.0f : 0.0f; break;
            case Op::Ge: v = (a >= b) ? 1.0f : 0.0f; break;
            case Op::And: v = (a != 0.0f && b != 0.0f) ? 1.0f : 0.0f; break;
            case Op::Or: v = (a != 0.0f || b != 0.0f) ? 1.0f : 0.0f; break;
            case Op::Xor: v = ((a != 0.0f) != (b != 0.0f)) ? 1.0f : 0.0f; break;
            default: break;
            }
            if (auto r = push(v); !r) return r;
            break;
        }

        case Op::Neg: case Op::Abs: case Op::Floor: case Op::Round:
        case Op::Sqrt: case Op::Not:
        case Op::Scale: case Op::Tx: case Op::Ty: {
            float a;
            if (auto r = pop(a); !r) return r;
            float v = 0.0f;
            switch (static_cast<Op>(op)) {
            case Op::Neg: v = -a; break;
            case Op::Abs: v = std::fabs(a); break;
            case Op::Floor: v = std::floor(a); break;
            case Op::Round: v = std::round(a); break;
            case Op::Sqrt: v = std::sqrt(a); break;
            case Op::Not: v = (a == 0.0f) ? 1.0f : 0.0f; break;
            case Op::Scale: { float s = a * scale; v = s < 1.0f ? 1.0f : s; break; }
            case Op::Tx: v = cx_screen + (a - 160.0f) * scale; break;
            case Op::Ty: v = cy_screen + (a - 120.0f) * scale; break;
            default: break;
            }
            if (auto r = push(v); !r) return r;
            break;
        }

        case Op::Clamp: {
            float hi, lo, x;
            if (auto r = pop(hi); !r) return r;
            if (auto r = pop(lo); !r) return r;
            if (auto r = pop(x); !r) return r;
            float v = x < lo ? lo : (x > hi ? hi : x);
            if (auto r = push(v); !r) return r;
            break;
        }

        case Op::Jmp: {
            if (pc + 2 > code_size) return tl::unexpected(VmError::Truncated);
            std::int16_t off = rd_i16(code + pc);
            pc += 2;
            if (auto r = jump(off, pc); !r) return r;
            break;
        }
        case Op::Jz: case Op::Jnz: {
            if (pc + 2 > code_size) return tl::unexpected(VmError::Truncated);
            std::int16_t off = rd_i16(code + pc);
            pc += 2;
            float a;
            if (auto r = pop(a); !r) return r;
            const bool take = (static_cast<Op>(op) == Op::Jz) ? (a == 0.0f) : (a != 0.0f);
            if (take) {
                if (auto r = jump(off, pc); !r) return r;
            }
            break;
        }

        case Op::Call: {
            if (pc + 1 > code_size) return tl::unexpected(VmError::Truncated);
            const std::uint8_t fn_id = code[pc++];
            if (fn_id >= bc.fns.size()) return tl::unexpected(VmError::BadFnId);
            const FnEntry& fe = bc.fns[fn_id];
            if (fp_ >= kMaxCallDepth) return tl::unexpected(VmError::CallDepthExceeded);
            Frame& parent = frames_[fp_ - 1];
            const std::uint16_t new_base =
                static_cast<std::uint16_t>(parent.locals_base + parent.locals_size);
            if (new_base + fe.local_count > kMaxLocals) return tl::unexpected(VmError::BadLocalSlot);
            if (sp_ < fe.param_count) return tl::unexpected(VmError::StackUnderflow);
            // Pop arguments into the callee's first `param_count` local slots
            // (left-to-right declaration order = bottom-to-top on the stack).
            for (int i = fe.param_count - 1; i >= 0; --i) {
                locals_[new_base + static_cast<std::size_t>(i)] = stack_[--sp_];
            }
            for (std::size_t i = fe.param_count; i < fe.local_count; ++i) {
                locals_[new_base + i] = 0.0f;
            }
            Frame& nf = frames_[fp_++];
            nf.return_pc = static_cast<std::uint16_t>(pc);
            nf.locals_base = new_base;
            nf.locals_size = fe.local_count;
            pc = fe.code_offset;
            break;
        }
        case Op::Ret: {
            if (fp_ == 0) return tl::unexpected(VmError::StackUnderflow);
            Frame& f = frames_[--fp_];
            if (fp_ == 0) {
                // Root frame returned — program complete.
                return {};
            }
            pc = f.return_pc;
            break;
        }

        case Op::FillRect: {
            float color, h, w, y, x;
            if (auto r = pop(color); !r) return r;
            if (auto r = pop(h); !r) return r;
            if (auto r = pop(w); !r) return r;
            if (auto r = pop(y); !r) return r;
            if (auto r = pop(x); !r) return r;
            canvas.fillRect(f2i(x), f2i(y), f2i(w), f2i(h), f2color(color));
            break;
        }
        case Op::FillCircle: {
            float color, r, cy, cx;
            if (auto rr = pop(color); !rr) return rr;
            if (auto rr = pop(r); !rr) return rr;
            if (auto rr = pop(cy); !rr) return rr;
            if (auto rr = pop(cx); !rr) return rr;
            canvas.fillCircle(f2i(cx), f2i(cy), f2i(r), f2color(color));
            break;
        }
        case Op::FillTriangle: {
            float color, y2, x2, y1, x1, y0, x0;
            if (auto r = pop(color); !r) return r;
            if (auto r = pop(y2); !r) return r;
            if (auto r = pop(x2); !r) return r;
            if (auto r = pop(y1); !r) return r;
            if (auto r = pop(x1); !r) return r;
            if (auto r = pop(y0); !r) return r;
            if (auto r = pop(x0); !r) return r;
            canvas.fillTriangle(f2i(x0), f2i(y0), f2i(x1), f2i(y1), f2i(x2), f2i(y2), f2color(color));
            break;
        }
        case Op::BeginGroup: {
            float h, w, y, x;
            if (auto r = pop(h); !r) return r;
            if (auto r = pop(w); !r) return r;
            if (auto r = pop(y); !r) return r;
            if (auto r = pop(x); !r) return r;
            canvas.begin_group(f2i(x), f2i(y), f2i(w), f2i(h));
            break;
        }
        case Op::EndGroup:
            canvas.end_group();
            break;

        default:
            return tl::unexpected(VmError::UnknownOpcode);
        }
    }
}

} // namespace stackchan::avatar_vm
