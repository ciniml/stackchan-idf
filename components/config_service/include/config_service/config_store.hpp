// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <config_service/config_service.hpp>
#include <tl/expected.hpp>

namespace stackchan::config::registry {
struct SettingDescriptor;
}

namespace stackchan::config::store {

DeviceConfig load();
tl::expected<void, Error> save(const DeviceConfig& cfg);

// Persist a single setting's current value (string or numeric) to NVS + commit,
// without touching any other key. The immediate-apply path (settings that take
// effect at runtime, no reboot) uses this so a per-key write neither hammers the
// whole blob nor risks the full save() clobbering a concurrent staged change.
tl::expected<void, Error> save_one(const registry::SettingDescriptor& d,
                                   const DeviceConfig& cfg);

// Persist just the LED tuple without re-writing every other NVS key. Used
// by the BLE/HTTP LED patch sink to avoid hammering wear-levelling on the
// shared "stackchan_cfg" namespace every time a slider moves.
tl::expected<void, Error> save_led_state(std::uint8_t mode, std::uint32_t color,
                                         std::uint8_t brightness,
                                         std::uint8_t gradient_period_ds);

// Persist just the mic-lip-sync gain pair (input gain pct / output gain pct).
// Same single-writer pattern as save_led_state — calibration sliders push
// updates here directly rather than going through the full save() Apply
// path so they take effect immediately and don't clobber other staging.
tl::expected<void, Error> save_mic_lip_gain(std::uint16_t input_pct,
                                            std::uint16_t output_pct);

// Persist just the speaker volume percent (0..200). Same single-writer
// pattern as save_mic_lip_gain — slider updates skip the full save()
// Apply path so they take effect immediately and survive reboot.
tl::expected<void, Error> save_speaker_volume(std::uint16_t pct);

} // namespace stackchan::config::store
