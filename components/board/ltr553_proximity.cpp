// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/ltr553_proximity.hpp"

#include <M5Unified.h>
#include <esp_log.h>

namespace stackchan::board {

namespace {

constexpr const char* kTag = "ltr553";

// Register map (LTR-553ALS-WA datasheet).
constexpr std::uint8_t kRegAlsContr = 0x80;
constexpr std::uint8_t kRegPsContr = 0x81;
constexpr std::uint8_t kRegPsLed = 0x82;
constexpr std::uint8_t kRegPsNPulses = 0x83;
constexpr std::uint8_t kRegPsMeasRate = 0x84;
constexpr std::uint8_t kRegPartId = 0x86;
constexpr std::uint8_t kRegManufacId = 0x87;
constexpr std::uint8_t kRegPsData0 = 0x8D; // low 8 bits
constexpr std::uint8_t kRegPsData1 = 0x8E; // [2:0] high bits, [7] saturation

constexpr std::uint8_t kPartId = 0x92;     // [7:4] part number 9, [3:0] revision 2
constexpr std::uint8_t kManufacId = 0x05;

constexpr std::uint8_t kRegPsOffset1 = 0x94;  // [2:0] high bits
constexpr std::uint8_t kRegPsOffset0 = 0x95;

// PS_CONTR: [1:0] = 11 active, [3:2] gain (00 x16, 10 x32, x1 x64),
// [5] saturation indicator enable.
constexpr std::uint8_t kPsContrStandby = 0x00;
constexpr std::uint8_t kGainBits[] = {0x0u << 2, 0x2u << 2, 0x1u << 2};
constexpr std::uint8_t kLedCurrentBits[] = {0, 1, 2, 3, 7};  // 5/10/20/50/100 mA
constexpr std::uint32_t kMeasPeriodMs[] = {50, 70, 100, 200, 500, 1000, 2000, 10};

template <typename T> T clamp_max(T v, T max) { return v > max ? max : v; }

} // namespace

bool Ltr553Proximity::write_register(std::uint8_t reg, std::uint8_t value)
{
    return m5::In_I2C.writeRegister8(address_, reg, value, kI2cFreq);
}

std::optional<std::uint8_t> Ltr553Proximity::read_register(std::uint8_t reg)
{
    std::uint8_t v = 0;
    if (!m5::In_I2C.readRegister(address_, reg, &v, 1, kI2cFreq)) {
        return std::nullopt;
    }
    return v;
}

tl::expected<Ltr553Proximity, Error> Ltr553Proximity::probe(std::uint8_t address)
{
    return probe(address, PsConfig{});
}

tl::expected<Ltr553Proximity, Error> Ltr553Proximity::probe(std::uint8_t address, const PsConfig& cfg)
{
    Ltr553Proximity chip{address};

    const auto part = chip.read_register(kRegPartId);
    const auto mfr = chip.read_register(kRegManufacId);
    if (!part || !mfr) {
        return tl::unexpected{Error::ProximityProbe};
    }
    if (*part != kPartId || *mfr != kManufacId) {
        ESP_LOGW(kTag, "unexpected ID at 0x%02X: part=0x%02X mfr=0x%02X", address, *part, *mfr);
        return tl::unexpected{Error::ProximityProbe};
    }

    if (!chip.configure(cfg)) {
        return tl::unexpected{Error::ProximityProbe};
    }
    return chip;
}

bool Ltr553Proximity::configure(const PsConfig& in)
{
    PsConfig c = in;
    c.gain = clamp_max(c.gain, kGainMax);
    c.led_freq = clamp_max(c.led_freq, kLedFreqMax);
    c.led_duty = clamp_max(c.led_duty, kLedDutyMax);
    c.led_current = clamp_max(c.led_current, kLedCurrentMax);
    c.pulses = c.pulses == 0 ? 1 : clamp_max(c.pulses, kPulsesMax);
    c.meas_rate = clamp_max(c.meas_rate, kMeasRateMax);
    c.offset = clamp_max(c.offset, kOffsetMax);

    const std::uint8_t led = static_cast<std::uint8_t>((c.led_freq << 5) | (c.led_duty << 3) | kLedCurrentBits[c.led_current]);
    const std::uint8_t rate = c.meas_rate == 7 ? 0x08u : c.meas_rate;
    const std::uint8_t contr = static_cast<std::uint8_t>(0x03u | kGainBits[c.gain] | (1u << 5));

    // Datasheet: ~100 ms from power-on to standby, and PS registers are only
    // writable in standby, so park PS, program everything, then go active.
    const bool ok = write_register(kRegPsContr, kPsContrStandby) &&
                    write_register(kRegPsLed, led) &&
                    write_register(kRegPsNPulses, c.pulses) &&
                    write_register(kRegPsMeasRate, rate) &&
                    write_register(kRegPsOffset1, static_cast<std::uint8_t>(c.offset >> 8)) &&
                    write_register(kRegPsOffset0, static_cast<std::uint8_t>(c.offset & 0xFF)) &&
                    write_register(kRegAlsContr, 0x00) && // ALS standby
                    write_register(kRegPsContr, contr);
    if (!ok) {
        ESP_LOGW(kTag, "configure failed (bus error)");
        return false;
    }
    cfg_ = c;
    static constexpr std::uint8_t kGainX[] = {16, 32, 64};
    static constexpr std::uint8_t kCurrentMa[] = {5, 10, 20, 50, 100};
    ESP_LOGI(kTag, "PS active at 0x%02X: gain x%u, LED %u mA %u%% %u kHz x%u pulses, %u ms, offset %u",
             address_, kGainX[c.gain], kCurrentMa[c.led_current], 25u * (c.led_duty + 1), 30u + 10u * c.led_freq,
             c.pulses, static_cast<unsigned>(kMeasPeriodMs[c.meas_rate]), c.offset);
    return true;
}

std::uint32_t Ltr553Proximity::period_ms() const noexcept
{
    return kMeasPeriodMs[cfg_.meas_rate];
}

std::optional<Ltr553Proximity::PsReading> Ltr553Proximity::read_ps()
{
    // Two-byte read starting at PS_DATA_0; the chip auto-increments and
    // latches DATA_1 so the pair is consistent.
    std::uint8_t buf[2] = {};
    if (!m5::In_I2C.readRegister(address_, kRegPsData0, buf, sizeof(buf), kI2cFreq)) {
        return std::nullopt;
    }
    PsReading r;
    r.raw = static_cast<std::uint16_t>(buf[0] | ((buf[1] & 0x07u) << 8));
    r.saturated = (buf[1] & 0x80u) != 0;
    return r;
}

} // namespace stackchan::board
