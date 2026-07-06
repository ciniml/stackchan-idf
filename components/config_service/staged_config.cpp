// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/staged_config.hpp>

namespace stackchan::config {

std::size_t StagedConfig::index_of(const registry::SettingDescriptor& d)
{
    return static_cast<std::size_t>(&d - registry::table().data());
}

void StagedConfig::set_str(const registry::SettingDescriptor& d, std::string value)
{
    Slot& s = slots_[index_of(d)];
    s.str = std::move(value);
    s.set = true;
}

void StagedConfig::set_num(const registry::SettingDescriptor& d, std::uint32_t value)
{
    Slot& s = slots_[index_of(d)];
    s.num = value;
    s.set = true;
}

void StagedConfig::set_str(std::string_view id, std::string value)
{
    if (const auto* d = registry::find(id)) set_str(*d, std::move(value));
}

void StagedConfig::set_num(std::string_view id, std::uint32_t value)
{
    if (const auto* d = registry::find(id)) set_num(*d, value);
}

bool StagedConfig::has(const registry::SettingDescriptor& d) const
{
    return slots_[index_of(d)].set;
}

void StagedConfig::merge_into(DeviceConfig& cfg) const
{
    const auto& tbl = registry::table();
    for (std::size_t i = 0; i < tbl.size(); ++i) {
        const Slot& s = slots_[i];
        if (!s.set) continue;
        const auto& d = tbl[i];
        if (d.type == registry::ValueType::Str) {
            cfg.*(d.str_member) = s.str;
        } else {
            d.num_set(cfg, s.num);
        }
    }
}

void StagedConfig::clear()
{
    for (Slot& s : slots_) {
        s.set = false;
        s.num = 0;
        s.str.clear();
    }
}

} // namespace stackchan::config
