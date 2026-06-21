// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <tl/expected.hpp>

#include "board/board.hpp"

namespace stackchan::board {

// M5Stack Module Audio (M144) — ES8388 codec + STM32 control MCU on the
// M-BUS stack.
//
// Pinout (verified against M144 schematic v10 + CoreS3 M5Unified internal
// pin tables, NOT the same as the internal AW88298 / ES7210):
//   MCLK = GPIO7, BCK = GPIO0, LRCK = GPIO6, DOUT (ESP→codec) = GPIO14,
//   DIN (codec→ESP, ADC) = GPIO13.
// This pinout COLLIDES with the CoreS3 internal mic ES7210 on GPIO0
// (internal MCLK) and GPIO14 (internal mic DIN), so the two paths are
// mutually exclusive — when Module Audio is fitted, the single I2S is
// re-routed to its pads and the internal AW88298 speaker / ES7210 mic
// stop receiving signal. Acceptable trade-off: Module Audio is fitted
// when you want venue-level / monitor audio, so the internal 1 W amp is
// redundant. App-side re-routing lives in main/app_main.cpp's
// M5.Speaker.config block (the has_audio_module branch).
//
// I2C addresses on the internal bus:
//   0x10 = ES8388 codec (this file's init / set_volume)
//   0x33 = STM32 MCU (LED strip 0x40..0x48, HP-detect 0x20, etc. — see
//          M144_Protocol.pdf for the full register map)
//
// The module has NO speaker amplifier — ES8388 LOUT/ROUT drives the
// TRRS jack directly through an FSUSB42MUX (CTIA/OMTP swap). Connect an
// active speaker or headphones for any audible output.
namespace es8388 {

constexpr std::uint8_t kI2cAddress = 0x10;
// On-module MCU (handles LEDs, buttons, possibly amp / mute control).
// Confirmed by the M5Stack-supplied schematic; sits at 0x33, one slot
// away from the forbidden AXP2101 (0x34) — but we only access 0x33
// explicitly, never via a bus scan, so the project rule holds.
constexpr std::uint8_t kMcuI2cAddress = 0x33;
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

// Diagnostic: write a recognizable RGB pattern (LED1=red, LED2=green,
// LED3=blue) to registers 0x40..0x48 of the Module Audio's on-board MCU
// (I2C 0x33). If I2C is reaching the module the LED strip lights up
// obviously; if it isn't, every byte gets NAK'd and we know to look at
// the M-BUS connection / jumper config rather than the ES8388 settings.
// Independent of ES8388 probe — useful even when the audio codec doesn't
// show up at 0x10, because the MCU lives on the same I2C bus.
tl::expected<void, Error> diagnose_rgb_pattern();

// Read the MCU's headphone-jack-insertion status from register 0x20.
// Returns true if a 3.5 mm plug is detected in the HP jack. The MCU's
// audio routing defaults already steer playback to whichever jack is
// active (line out + HP), so the firmware doesn't need to act on this —
// but logging it at boot helps confirm the MCU is alive and the user has
// the cable connected to the jack they think they have.
tl::expected<bool, Error> headphone_inserted();

} // namespace es8388

} // namespace stackchan::board
