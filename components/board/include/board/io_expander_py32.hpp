#pragma once

#include <cstdint>
#include <expected>

#include "board/board.hpp"

namespace stackchan::board {

class Py32Expander {
public:
    static constexpr std::uint8_t kAddress = 0x6F;
    static constexpr std::uint32_t kI2cFreq = 100'000;

    static constexpr std::uint8_t kPinServoPowerEnable = 0;

    static std::expected<Py32Expander, Error> probe(std::uint8_t address = kAddress);

    std::expected<void, Error> set_direction(std::uint8_t pin, bool output);
    std::expected<void, Error> set_pull_up(std::uint8_t pin, bool enable);
    std::expected<void, Error> digital_write(std::uint8_t pin, bool level);

    std::uint8_t address() const noexcept { return address_; }

private:
    explicit Py32Expander(std::uint8_t address) noexcept : address_{address} {}

    std::expected<void, Error> write_bit(std::uint8_t reg_l, std::uint8_t reg_h, std::uint8_t pin, bool value);

    std::uint8_t address_;
};

} // namespace stackchan::board
