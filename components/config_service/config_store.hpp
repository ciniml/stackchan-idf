// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <config_service/config_service.hpp>
#include <tl/expected.hpp>

namespace stackchan::config::store {

DeviceConfig load();
tl::expected<void, Error> save(const DeviceConfig& cfg);

} // namespace stackchan::config::store
