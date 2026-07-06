// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <config_service/settings_registry.hpp>

namespace stackchan::config {

// Registry-driven staging buffer: values written through the settings
// transports (BLE / HTTP) accumulate here until Apply, when merge_into()
// folds them over the boot-time DeviceConfig snapshot. Replaces the two
// per-transport `StagingBuffer` structs that mirrored DeviceConfig with a
// hand-maintained std::optional<> per field. Each transport still owns its
// own instance — staging stays per-transport, only the mechanism is shared.
//
// Not thread-safe by itself; callers guard with their existing transport
// mutex exactly as they did around the old struct.
class StagedConfig {
public:
    // Stage a value for the given descriptor. The str/num split mirrors
    // SettingDescriptor: Str rows take set_str, everything else set_num
    // (Bool as 0/1, enums as their wire byte).
    void set_str(const registry::SettingDescriptor& d, std::string value);
    void set_num(const registry::SettingDescriptor& d, std::uint32_t value);

    // Convenience: look the descriptor up by registry id first. No-ops on an
    // unknown id (ids at call sites are compile-time literals, so a miss is a
    // programming error, not runtime input).
    void set_str(std::string_view id, std::string value);
    void set_num(std::string_view id, std::uint32_t value);

    bool has(const registry::SettingDescriptor& d) const;

    // Fold every staged value over `cfg`, leaving unstaged fields untouched.
    void merge_into(DeviceConfig& cfg) const;

    void clear();

private:
    struct Slot {
        bool set = false;
        std::uint32_t num = 0;
        std::string str;
    };
    static std::size_t index_of(const registry::SettingDescriptor& d);
    std::array<Slot, registry::kSettingCount> slots_{};
};

} // namespace stackchan::config
