#pragma once

#include <cstdint>
#include <expected>

#include "scs_servo/scs_bus.hpp"
#include "scs_servo/scs_error.hpp"

namespace stackchan::scs_servo {

// SCS0009 default servo IDs and zero positions for Stack-chan CoreS3 base.
inline constexpr std::uint8_t kYawId = 1;
inline constexpr std::uint8_t kPitchId = 2;
inline constexpr std::uint16_t kYawZero = 460;
inline constexpr std::uint16_t kPitchZero = 620;

// 1 step ≈ 0.3125°  (deg = (raw - zero) * 5 / 16)
constexpr float raw_to_deg(std::uint16_t raw, std::uint16_t zero) noexcept
{
    return (static_cast<int>(raw) - static_cast<int>(zero)) * 5.0f / 16.0f;
}

constexpr std::uint16_t deg_to_raw(float deg, std::uint16_t zero) noexcept
{
    const float r = deg * 16.0f / 5.0f + static_cast<float>(zero);
    if (r < 0.0f) {
        return 0;
    }
    if (r > 1023.0f) {
        return 1023;
    }
    return static_cast<std::uint16_t>(r);
}

class ScsServo {
public:
    ScsServo(ScsBus& bus, std::uint8_t id) noexcept : bus_{bus}, id_{id} {}

    std::expected<void, ScsError> ping();
    std::expected<void, ScsError> enable_torque(bool on);
    std::expected<void, ScsError> write_goal_position(std::uint16_t raw, std::uint16_t time_ms,
                                                     std::uint16_t speed);
    std::expected<std::uint16_t, ScsError> read_present_position();

    std::uint8_t id() const noexcept { return id_; }

private:
    ScsBus& bus_;
    std::uint8_t id_;
};

} // namespace stackchan::scs_servo
