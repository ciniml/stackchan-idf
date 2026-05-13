// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "scs_servo/scs_servo.hpp"

#include <array>

namespace stackchan::scs_servo {

namespace {

constexpr std::uint8_t kInstPing = 0x01;
constexpr std::uint8_t kInstRead = 0x02;
constexpr std::uint8_t kInstWrite = 0x03;

constexpr std::uint8_t kRegTorqueEnable = 0x28;
constexpr std::uint8_t kRegGoalPositionLow = 0x2A;
constexpr std::uint8_t kRegPresentPositionLow = 0x38;

} // namespace

tl::expected<void, ScsError> ScsServo::ping()
{
    std::array<std::uint8_t, 4> scratch{};
    auto r = bus_.transact(id_, kInstPing, {}, scratch);
    if (!r) {
        return tl::unexpected{r.error()};
    }
    return {};
}

tl::expected<void, ScsError> ScsServo::enable_torque(bool on)
{
    const std::array<std::uint8_t, 2> params{kRegTorqueEnable, static_cast<std::uint8_t>(on ? 1 : 0)};
    std::array<std::uint8_t, 4> scratch{};
    auto r = bus_.transact(id_, kInstWrite, params, scratch);
    if (!r) {
        return tl::unexpected{r.error()};
    }
    return {};
}

tl::expected<void, ScsError>
ScsServo::write_goal_position(std::uint16_t raw, std::uint16_t time_ms, std::uint16_t speed)
{
    // SCSCL series uses big-endian wire format (high byte first) for multi-byte
    // register values. See FTServo_Arduino SCS::Host2SCS with End=1.
    const std::array<std::uint8_t, 7> params{
        kRegGoalPositionLow,
        static_cast<std::uint8_t>((raw >> 8) & 0xFF),
        static_cast<std::uint8_t>(raw & 0xFF),
        static_cast<std::uint8_t>((time_ms >> 8) & 0xFF),
        static_cast<std::uint8_t>(time_ms & 0xFF),
        static_cast<std::uint8_t>((speed >> 8) & 0xFF),
        static_cast<std::uint8_t>(speed & 0xFF),
    };
    std::array<std::uint8_t, 4> scratch{};
    auto r = bus_.transact(id_, kInstWrite, params, scratch);
    if (!r) {
        return tl::unexpected{r.error()};
    }
    return {};
}

tl::expected<std::uint16_t, ScsError> ScsServo::read_present_position()
{
    const std::array<std::uint8_t, 2> params{kRegPresentPositionLow, 2};
    std::array<std::uint8_t, 4> scratch{};
    auto r = bus_.transact(id_, kInstRead, params, scratch);
    if (!r) {
        return tl::unexpected{r.error()};
    }
    if (r->size() < 2) {
        return tl::unexpected{ScsError::BadLength};
    }
    // Big-endian response: high byte first.
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>((*r)[0]) << 8) | (*r)[1]);
}

} // namespace stackchan::scs_servo
