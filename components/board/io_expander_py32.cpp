#include "board/io_expander_py32.hpp"

#include <M5Unified.h>

namespace stackchan::board {

namespace {

constexpr std::uint8_t kRegVersion = 0x02;
constexpr std::uint8_t kRegGpioModeLow = 0x03;
constexpr std::uint8_t kRegGpioModeHigh = 0x04;
constexpr std::uint8_t kRegGpioOutLow = 0x05;
constexpr std::uint8_t kRegGpioOutHigh = 0x06;
constexpr std::uint8_t kRegGpioPullUpLow = 0x09;
constexpr std::uint8_t kRegGpioPullUpHigh = 0x0A;
constexpr std::uint8_t kRegGpioPullDownLow = 0x0B;
constexpr std::uint8_t kRegGpioPullDownHigh = 0x0C;

} // namespace

std::expected<Py32Expander, Error> Py32Expander::probe(std::uint8_t address)
{
    const std::uint8_t version = m5::In_I2C.readRegister8(address, kRegVersion, kI2cFreq);
    if (version == 0x00 || version == 0xFF) {
        return std::unexpected{Error::ExpanderProbe};
    }
    return Py32Expander{address};
}

std::expected<void, Error> Py32Expander::write_bit(std::uint8_t reg_l, std::uint8_t reg_h, std::uint8_t pin, bool value)
{
    const std::uint8_t reg = (pin < 8) ? reg_l : reg_h;
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << (pin & 0x7));

    const std::uint8_t current = m5::In_I2C.readRegister8(address_, reg, kI2cFreq);
    const std::uint8_t next = value ? static_cast<std::uint8_t>(current | mask) : static_cast<std::uint8_t>(current & ~mask);
    if (!m5::In_I2C.writeRegister8(address_, reg, next, kI2cFreq)) {
        return std::unexpected{Error::ExpanderWrite};
    }
    return {};
}

std::expected<void, Error> Py32Expander::set_direction(std::uint8_t pin, bool output)
{
    return write_bit(kRegGpioModeLow, kRegGpioModeHigh, pin, output);
}

std::expected<void, Error> Py32Expander::set_pull_up(std::uint8_t pin, bool enable)
{
    // Clear pull-down first to avoid pull-up/down conflict, then set pull-up.
    if (auto r = write_bit(kRegGpioPullDownLow, kRegGpioPullDownHigh, pin, false); !r) {
        return r;
    }
    return write_bit(kRegGpioPullUpLow, kRegGpioPullUpHigh, pin, enable);
}

std::expected<void, Error> Py32Expander::digital_write(std::uint8_t pin, bool level)
{
    return write_bit(kRegGpioOutLow, kRegGpioOutHigh, pin, level);
}

} // namespace stackchan::board
