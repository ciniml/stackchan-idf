// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

namespace stackchan::config::dis {

// Register the standard Device Information Service (UUID 0x180A) with the
// NimBLE GATT server. Must be called after nimble_port_init() and before
// nimble_port_freertos_init(). The Firmware Revision characteristic is
// populated from esp_app_get_description()->version, which CMake fills with
// `git describe --always --tags --dirty` at configure time.
void init();

} // namespace stackchan::config::dis
