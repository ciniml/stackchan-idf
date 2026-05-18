// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace stackchan::config::ota {

// JSON command set served by the OtaControl GATT characteristic:
//   {"op":"begin","size":<bytes>}   open the next OTA partition
//   {"op":"end"}                    finalise + set_boot + schedule reboot
//   {"op":"abort"}                  cancel an in-progress transfer
// Returns a JSON document describing the outcome (always non-empty).
std::string handle_control_command(const std::string& json);

// Append a chunk of firmware bytes (already decrypted from the BLE session)
// to the in-progress OTA partition. Returns the JSON status snapshot or an
// error document. Calling without an active begin() returns an error.
std::string handle_data_chunk(std::span<const std::uint8_t> data);

// Current state — also what an OtaControl READ returns.
//   {"state":"idle"|"receiving"|"done"|"failed",
//    "received":<bytes>,"total":<bytes>,"error":"<msg>"}
std::string status_json();

// Drop any in-progress transfer (esp_ota_abort). Call on BLE disconnect so
// a half-finished image can never be marked bootable.
void abort();

} // namespace stackchan::config::ota
