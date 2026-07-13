// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <config_service/config_service.hpp>

namespace stackchan::config::registry {

// Storage / wire width of a setting's value. Strings go through nvs_*_str;
// Bool is a u8 0/1; the numeric widths map 1:1 to the nvs_get/set_u8/16/32
// calls so the on-flash blob types stay exactly what they were before the
// registry existed.
enum class ValueType : std::uint8_t {
    Str,
    Bool,
    U8,
    U16,
    U32,
};

// How a change to this setting reaches the running firmware:
//   Staged — sits in the staging buffer until Apply, then full-save + reboot.
//   Live   — applied immediately through a sink and persisted by its own
//            single-writer NVS save (save_led_state / save_mic_lip_gain /
//            save_speaker_volume). EXCLUDED from the full save() so an Apply
//            can't clobber a runtime change with the stale boot value.
//   Both   — staged for Apply AND live-applied on write (face / LT config).
//            Persisted via the full save() on Apply (NOT independently).
//   Immediate — applied at runtime AND persisted independently per-key via
//            store::save_one on write (apply_immediate_* / on_config_change).
//            Like Live, EXCLUDED from the full save() so a stale-g_active Apply
//            from *either* transport can't clobber the runtime value — this is
//            what makes cross-transport (HTTP-immediate + BLE-Apply) safe. Also
//            counts as "no reboot needed" for the UI.
enum class ApplyKind : std::uint8_t {
    Staged,
    Live,
    Both,
    Immediate,
};

// One row per DeviceConfig field. `id` is the stable external identifier
// (matches the existing per-key HTTP route suffix where one exists) and is
// itself a wire contract once the batch settings API ships. `nvs_key` is the
// on-flash key — renaming one loses existing installs' settings, so the
// whole column is a WIRE CONTRACT: append rows, never rename.
struct SettingDescriptor {
    const char* id;
    const char* nvs_key;
    ValueType type;
    ApplyKind apply;
    // Value never leaves the device (API keys, passwords) — readers expose
    // only a has_<id> flag.
    bool secret;
    // Out-of-range numeric handling on load: clamp to max_value (true) or
    // keep the built-in default (false). Irrelevant when max_value == 0.
    bool clamp;
    // Inclusive numeric ceiling; 0 = unbounded (full width accepted).
    std::uint32_t max_value;
    // Str rows: maximum plaintext byte length accepted over the wire.
    // Mirrors the per-transport kMax* constants; 0 for numeric rows.
    std::uint16_t max_len;
    // Str fields use the member pointer; numeric fields (Bool/U8/U16/U32)
    // round-trip through u32 via the accessor pair. Exactly one side is set.
    std::string DeviceConfig::* str_member;
    std::uint32_t (*num_get)(const DeviceConfig&);
    void (*num_set)(DeviceConfig&, std::uint32_t);
};

inline constexpr std::size_t kSettingCount = 35;

// The full table, one row per DeviceConfig field. Order is stable but not a
// contract (lookup is by id / nvs_key).
const std::array<SettingDescriptor, kSettingCount>& table();

// nullptr when no row matches.
const SettingDescriptor* find(std::string_view id);
const SettingDescriptor* find_by_nvs_key(std::string_view nvs_key);

// A setting needs a reboot to take effect iff it is staged-only. Immediate
// settings (ApplyKind::Live / ApplyKind::Both) reach the running firmware on
// write (SharedState store / sink / apply_immediate_*), so they never require a
// restart. Used to surface a "restart required" hint to the settings UI.
inline bool needs_reboot(const SettingDescriptor& d)
{
    return d.apply == ApplyKind::Staged;
}

} // namespace stackchan::config::registry
