// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <sdkconfig.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>

namespace stackchan {

// Stack caps for tasks that never call flash / NVS / OTA APIs (the only
// thing that forces a stack into internal RAM): PSRAM when the board has
// it, internal RAM otherwise. A bare MALLOC_CAP_SPIRAM request on a
// PSRAM-less board (AtomS3, CONFIG_SPIRAM=n) fails and the task is silently
// never created — render / led / servo / speech all vanished that way.
#if CONFIG_SPIRAM
inline constexpr UBaseType_t kNoFlashTaskStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
inline constexpr UBaseType_t kNoFlashTaskStackCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif

} // namespace stackchan
