// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <optional>
#include <tl/expected.hpp>

#include "board/board.hpp"

namespace stackchan::board {

// LiteOn LTR-553ALS-WA ambient-light + proximity sensor on the CoreS3
// mainboard (internal I²C 0x23, next to the LCD's top edge). M5Unified lists
// the part in its device table but ships no driver, so this is ours. Same
// bus as PMIC / IO expander / Si12T touch → all access goes through
// m5::In_I2C, and periodic reads must respect SharedState::i2c_quiesce
// (camera sessions borrow the bus pins).
//
// Only the proximity channel (PS) is configured here. PS_DATA is an 11-bit
// reflected-IR count (0..2047): monotonic in "how close" but non-linear and
// dependent on the target's reflectivity and ambient IR, so thresholds are
// runtime-tunable (settings), not constants.
class Ltr553Proximity {
public:
    static constexpr std::uint8_t kAddress = 0x23;
    static constexpr std::uint32_t kI2cFreq = 100'000;
    static constexpr std::uint16_t kPsMax = 2047;

    struct PsReading {
        std::uint16_t raw = 0;   // 0..2047
        bool saturated = false;  // PS_DATA_1 bit7: target too close / too reflective
    };

    // Tunable PS front-end (datasheet §PS_CONTR / PS_LED / PS_N_PULSES /
    // PS_MEAS_RATE / PS_OFFSET). Every field is an index into the chip's
    // value table so the wire format (settings / BLE) is a small integer:
    //   gain        0 = x16, 1 = x32, 2 = x64                (receiver gain)
    //   led_freq    0..7 = 30, 40, 50, 60, 70, 80, 90, 100 kHz
    //   led_duty    0..3 = 25, 50, 75, 100 %
    //   led_current 0..4 = 5, 10, 20, 50, 100 mA
    //   pulses      1..15
    //   meas_rate   0..6 = 50, 70, 100, 200, 500, 1000, 2000 ms; 7 = 10 ms
    //   offset      0..1023, subtracted from the raw count (crosstalk cancel)
    // Changing the emitter / gain rescales PS_DATA, so the near / far
    // thresholds have to be re-tuned afterwards — that's what the settings
    // page's 調整モード is for.
    struct PsConfig {
        std::uint8_t gain = 0;
        std::uint8_t led_freq = 3;
        std::uint8_t led_duty = 3;
        std::uint8_t led_current = 4;
        std::uint8_t pulses = 8;
        std::uint8_t meas_rate = 2;
        std::uint16_t offset = 0;
    };
    static constexpr std::uint8_t kGainMax = 2, kLedFreqMax = 7, kLedDutyMax = 3, kLedCurrentMax = 4,
                                  kPulsesMax = 15, kMeasRateMax = 7;
    static constexpr std::uint16_t kOffsetMax = 1023;

    // Verifies PART_ID / MANUFAC_ID, then applies `cfg` and puts the PS
    // channel in active mode. Leaves ALS in standby.
    static tl::expected<Ltr553Proximity, Error> probe(std::uint8_t address = kAddress);
    static tl::expected<Ltr553Proximity, Error> probe(std::uint8_t address, const PsConfig& cfg);

    // Re-program the PS front-end (standby → registers → active). Out-of-
    // range fields are clamped. Returns false on a bus error.
    bool configure(const PsConfig& cfg);
    const PsConfig& config() const noexcept { return cfg_; }

    // PS measurement period in ms for the current meas_rate setting.
    std::uint32_t period_ms() const noexcept;

    // Latest PS sample. std::nullopt on bus error (NACK) — callers keep their
    // previous state rather than treating it as "far".
    std::optional<PsReading> read_ps();

    // Fastest poll period the loop uses regardless of meas_rate.
    static constexpr std::uint32_t kPsPeriodMs = 100;

    std::uint8_t address() const noexcept { return address_; }

private:
    explicit Ltr553Proximity(std::uint8_t address) noexcept : address_{address} {}

    bool write_register(std::uint8_t reg, std::uint8_t value);
    std::optional<std::uint8_t> read_register(std::uint8_t reg);

    std::uint8_t address_;
    PsConfig cfg_;
};

} // namespace stackchan::board
