// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/audio_module_es8388.hpp"

#include <M5Unified.h>
#include <esp_log.h>

namespace stackchan::board::es8388 {

namespace {

constexpr const char* kTag = "es8388";

struct RegVal {
    std::uint8_t reg;
    std::uint8_t val;
};

// DAC-playback init, adapted from M5Unified's Tab5 ES8388 bring-up
// (M5Unified.cpp _speaker_enabled_cb_tab5) — the register sequence is
// chip-generic; only the surrounding amp-enable GPIO handling was Tab5
// specific (Module Audio has no amp, so none here).
constexpr RegVal kInitSequence[] = {
    {0, 0x80},  // CONTROL1: reset
    {0, 0x00},  // CONTROL1: normal operation
    {0, 0x0E},  // CONTROL1: play&record mode, enable reference
    {1, 0x00},  // CONTROL2: power up analog & ibiasgen
    {2, 0x0A},  // CHIPPOWER: power up (stop when in standby)
    {3, 0xFF},  // ADCPOWER: ADC fully powered down (playback only)
    {4, 0x3C},  // DACPOWER: DAC up, LOUT1/ROUT1 + LOUT2/ROUT2 enabled
    {5, 0x00},  // CHIPLOPOW1
    {6, 0x00},  // CHIPLOPOW2
    {7, 0x7C},  // ANAVOLMANAG: VSEL
    {8, 0x00},  // MASTERMODE: I2S slave
    {23, 0x18}, // DACCONTROL1: I2S 16-bit
    {24, 0x00}, // DACCONTROL2: MCLK/fs = 256 auto
    {25, 0x20}, // DACCONTROL3: DAC unmute
    {26, 0x00}, // LDACVOL: 0 dB
    {27, 0x00}, // RDACVOL: 0 dB
    {28, 0x08}, // DACCONTROL4: click-free power up/down
    {29, 0x00}, // DACCONTROL5
    {38, 0x00}, // DACCONTROL16: mixer select
    {39, 0xB8}, // DACCONTROL17: left DAC -> left mixer
    {42, 0xB8}, // DACCONTROL20: right DAC -> right mixer
    {43, 0x08}, // DACCONTROL21: ADC and DAC use separate LRCK
    {45, 0x00}, // DACCONTROL23: VREF 1.5k
};

constexpr std::uint8_t kRegLOut1Vol = 46;
constexpr std::uint8_t kRegROut1Vol = 47;
constexpr std::uint8_t kRegLOut2Vol = 48;
constexpr std::uint8_t kRegROut2Vol = 49;
constexpr std::uint8_t kMaxVol = 33; // 0x21, ≈ +4.5 dB

} // namespace

bool probe()
{
    return m5::In_I2C.scanID(kI2cAddress, kI2cFreq);
}

tl::expected<void, Error> init()
{
    for (const auto& rv : kInitSequence) {
        if (!m5::In_I2C.writeRegister8(kI2cAddress, rv.reg, rv.val, kI2cFreq)) {
            ESP_LOGE(kTag, "init write failed at reg %u", rv.reg);
            return tl::unexpected{Error::ExpanderWrite};
        }
    }
    // Default to the codec's max line-out gain (+4.5 dB). The module has
    // no power amplifier so anything driven directly into a TRRS load is
    // line-level — picking the codec's ceiling here gives the most
    // headroom for downstream speaker / headphone amps without adding
    // digital pre-gain (which would risk DAC clipping). Callers can dial
    // back via set_volume() if a loud powered speaker is wired in.
    return set_volume(33);
}

tl::expected<void, Error> set_volume(std::uint8_t vol_0_33)
{
    if (vol_0_33 > kMaxVol) vol_0_33 = kMaxVol;
    for (std::uint8_t reg : {kRegLOut1Vol, kRegROut1Vol, kRegLOut2Vol, kRegROut2Vol}) {
        if (!m5::In_I2C.writeRegister8(kI2cAddress, reg, vol_0_33, kI2cFreq)) {
            ESP_LOGE(kTag, "volume write failed at reg %u", reg);
            return tl::unexpected{Error::ExpanderWrite};
        }
    }
    return {};
}

tl::expected<void, Error> diagnose_rgb_pattern()
{
    // Target the Module Audio MCU at 0x33 (handles LED strip / buttons).
    // LED1 = red, LED2 = green, LED3 = blue — each LED gets 3 bytes
    // (R, G, B) packed contiguously at 0x40 / 0x43 / 0x46.
    static constexpr struct {
        std::uint8_t reg;
        std::uint8_t val;
    } kPattern[] = {
        {0x40, 0xFF}, {0x41, 0x00}, {0x42, 0x00}, // LED1 red
        {0x43, 0x00}, {0x44, 0xFF}, {0x45, 0x00}, // LED2 green
        {0x46, 0x00}, {0x47, 0x00}, {0x48, 0xFF}, // LED3 blue
    };
    bool any_failure = false;
    for (const auto& p : kPattern) {
        const bool ok = m5::In_I2C.writeRegister8(kMcuI2cAddress, p.reg, p.val, kI2cFreq);
        ESP_LOGI(kTag, "diag MCU write reg=0x%02X val=0x%02X -> %s",
                 p.reg, p.val, ok ? "ACK" : "NAK");
        if (!ok) any_failure = true;
    }
    if (any_failure) {
        ESP_LOGW(kTag, "diag: at least one register write was NAK'd at MCU 0x%02X "
                       "(check M-BUS / jumpers)", kMcuI2cAddress);
        return tl::unexpected{Error::ExpanderWrite};
    }
    ESP_LOGI(kTag, "diag: all 9 register writes ACK'd at MCU 0x%02X — I2C reachable",
             kMcuI2cAddress);
    return {};
}

tl::expected<bool, Error> headphone_inserted()
{
    std::uint8_t v = 0;
    if (!m5::In_I2C.readRegister(kMcuI2cAddress, 0x20, &v, 1, kI2cFreq)) {
        ESP_LOGW(kTag, "HP insertion read NAK at MCU 0x%02X reg 0x20", kMcuI2cAddress);
        return tl::unexpected{Error::ExpanderWrite};
    }
    // Non-zero = inserted (we don't know which bit pattern the MCU uses for
    // multi-bit status, so just non-zero is the safe interpretation).
    return v != 0;
}

} // namespace stackchan::board::es8388
