// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

namespace stackchan::app {

// Diagnostic helper: read-only dump of the registers on every chip we know
// is sitting on M5's internal I2C bus (AXP2101 / AW9523 / PY32). Lines are
// emitted at ESP_LOG_INFO under tag "i2c_dump", 16 bytes per line in the
// classic `xxd` layout so it's easy to diff before / after a boot or to
// compare against the chip's datasheet defaults.
//
// IMPORTANT: this only READS — no writes. The earlier LCD-backlight outage
// came from a stray I2C scan touching AXP2101 with bad write data; deliberate
// reads on documented addresses have no such side effect.
//
// Call after M5.begin() (so the I2C bus is configured) but BEFORE any LED /
// PY32 access so the dump reflects the boot state the corruption left behind.
void dump_internal_i2c_registers();

} // namespace stackchan::app
