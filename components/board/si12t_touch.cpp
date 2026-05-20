// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/si12t_touch.hpp"

#include <M5Unified.h>
#include <esp_log.h>

namespace stackchan::board {

namespace {

constexpr const char* kTag = "si12t";

// Register map (matches StackChan-BSP Si12T driver).
constexpr std::uint8_t kRegSensitivity1 = 0x02;
constexpr std::uint8_t kRegSensitivity5 = 0x06;
constexpr std::uint8_t kRegCtrl1 = 0x08;
constexpr std::uint8_t kRegCtrl2 = 0x09;
constexpr std::uint8_t kRegRefRst1 = 0x0A;
constexpr std::uint8_t kRegRefRst2 = 0x0B;
constexpr std::uint8_t kRegChHold1 = 0x0C;
constexpr std::uint8_t kRegChHold2 = 0x0D;
constexpr std::uint8_t kRegCalHold1 = 0x0E;
constexpr std::uint8_t kRegCalHold2 = 0x0F;
constexpr std::uint8_t kRegOutput1 = 0x10;

// Sensitivity nibble (each nibble configures one pair of channels; same
// value in both nibbles). Tuned downward over time:
//   - 0xCC ("High Level 4", BSP default) tripped Level-1 from EMI alone
//   - 0xBB ("High Level 3") still let Wi-Fi + BLE + radio coex bursts
//     drag every electrode up to Level-2 simultaneously, so the chip
//     output stuck at `front=2 middle=2 back=2` whenever the radios
//     were busy — visible as continuous diagnostic logs and a few
//     near-misses of the nadenade trigger before the all-equal exclusion
//     was added
//   - 0xAA ("High Level 2") raises the touch detection floor enough
//     that uniform EMI lift can't reach Level-2. Real touches still
//     resolve since fingertip capacitance dwarfs the EMI bias.
constexpr std::uint8_t kSensitivityHighLevel = 0xAA;

// CTRL1 = Auto Mode, FTC=01, response interrupt on Middle/High.
constexpr std::uint8_t kCtrl1Value = 0x22;

} // namespace

bool Si12tTouch::write_register(std::uint8_t reg, std::uint8_t value)
{
    return m5::In_I2C.writeRegister8(address_, reg, value, kI2cFreq);
}

tl::expected<Si12tTouch, Error> Si12tTouch::probe(std::uint8_t address)
{
    Si12tTouch chip{address};

    // Order matches the BSP's begin(): enable channels, then ctrl2 (S/W
    // reset + sleep toggle), then ctrl1, then sensitivity.
    constexpr std::uint8_t kEnableRegs[] = {
        kRegRefRst1, kRegRefRst2, kRegChHold1, kRegChHold2, kRegCalHold1, kRegCalHold2,
    };
    for (std::uint8_t reg : kEnableRegs) {
        if (!chip.write_register(reg, 0x00)) {
            ESP_LOGE(kTag, "Si12T init write 0x%02X failed", reg);
            return tl::unexpected{Error::TouchProbe};
        }
    }

    // CTRL2: S/W reset (0x0F) then sleep mode enable (0x07).
    if (!chip.write_register(kRegCtrl2, 0x0F) || !chip.write_register(kRegCtrl2, 0x07)) {
        ESP_LOGE(kTag, "Si12T CTRL2 write failed");
        return tl::unexpected{Error::TouchProbe};
    }
    if (!chip.write_register(kRegCtrl1, kCtrl1Value)) {
        ESP_LOGE(kTag, "Si12T CTRL1 write failed");
        return tl::unexpected{Error::TouchProbe};
    }
    for (std::uint8_t reg = kRegSensitivity1; reg <= kRegSensitivity5; ++reg) {
        if (!chip.write_register(reg, kSensitivityHighLevel)) {
            ESP_LOGE(kTag, "Si12T sensitivity write 0x%02X failed", reg);
            return tl::unexpected{Error::TouchProbe};
        }
    }

    ESP_LOGI(kTag, "Si12T touch sensor initialised at 0x%02X", address);
    return chip;
}

Si12tTouch::Reading Si12tTouch::read()
{
    Reading r{};
    std::uint8_t raw = 0;
    if (!m5::In_I2C.readRegister(address_, kRegOutput1, &raw, 1, kI2cFreq)) {
        return r;
    }
    // OUTPUT1 layout: bits[1:0] = back, bits[3:2] = middle, bits[5:4] = front
    // (BSP maps point_type[0]=bits0-1 then reverses to _intensities[2-i]).
    r.intensities[static_cast<std::size_t>(Zone::Back)] = (raw >> 0) & 0x03;
    r.intensities[static_cast<std::size_t>(Zone::Middle)] = (raw >> 2) & 0x03;
    r.intensities[static_cast<std::size_t>(Zone::Front)] = (raw >> 4) & 0x03;
    return r;
}

} // namespace stackchan::board
