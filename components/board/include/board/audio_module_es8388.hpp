// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <tl/expected.hpp>

#include "board/board.hpp"

namespace stackchan::board {

// M5Stack Module Audio (M144) — ES8388 codec on the M-BUS stack.
//
// With the module's jumpers in "Config B" (the CoreS3 position) the codec
// shares the host's existing I2S1 bus: BCK=GPIO34, WS=GPIO33, DIN=GPIO13 —
// the exact pins M5.Speaker already drives for the internal AW88298. The
// only host-side additions needed are
//   1. MCLK output on GPIO0 (the AW88298 doesn't use MCLK so the default
//      speaker config leaves it unset; the ES8388 requires it), and
//   2. a one-time I2C register init (this file).
// After that every M5.Speaker.playRaw()/tone() — conversation audio, jtts
// babble, /mcp/say, LT-timer announcements — comes out of the module's
// 3.5 mm line/headphone jacks in parallel with the internal speaker.
// NOTE: the module has NO speaker amplifier; connect an active (powered)
// speaker to the jack for venue-level volume.
namespace es8388 {

constexpr std::uint8_t kI2cAddress = 0x10;
constexpr std::uint32_t kI2cFreq = 100'000;

// True if an ES8388 ACKs on the internal I2C bus (single address probe, no
// register writes — safe alongside the "no blind I2C scans" project rule
// because it touches only 0x10, far from the AXP2101 at 0x34).
bool probe();

// Program the codec for I2S-slave 16-bit DAC playback, outputs LOUT1/ROUT1
// (headphone jack) + LOUT2/ROUT2 (line jack) enabled, ADC powered down.
// Call once after M5.begin(); independent of whether the I2S clocks are
// running yet (register writes need only I2C).
tl::expected<void, Error> init();

// Output volume for both jacks, 0..33 (1.5 dB steps; 30 ≈ 0 dB, 33 = max).
tl::expected<void, Error> set_volume(std::uint8_t vol_0_33);

} // namespace es8388

} // namespace stackchan::board
