// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "diag.hpp"

#include <cstdint>

#include <sdkconfig.h>

#include <esp_heap_caps.h>
#if CONFIG_HEAP_TASK_TRACKING
#include <esp_heap_task_info.h>
#endif
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "diag";

// Called by the heap on ANY failed allocation. ESP_EARLY_LOG* because this
// can fire from timing-sensitive contexts; keep the handler dead simple.
void on_alloc_fail(std::size_t size, std::uint32_t caps, const char* function_name)
{
    ESP_EARLY_LOGE(kTag,
                   "ALLOC_FAIL size=%u caps=0x%08x caller=%s | INT free=%u largest=%u | DMA largest=%u",
                   static_cast<unsigned>(size), static_cast<unsigned>(caps),
                   function_name != nullptr ? function_name : "?",
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
}

} // namespace

void diag_register_alloc_fail_hook()
{
    const esp_err_t err = heap_caps_register_failed_alloc_callback(&on_alloc_fail);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "failed_alloc_callback registration failed: %s", esp_err_to_name(err));
    }
}

void diag_heap(const char* label)
{
    ESP_LOGI(kTag, "[%s] INT free=%u largest=%u min=%u | DMA largest=%u | PSRAM free=%u",
             label != nullptr ? label : "-",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void diag_stack_hwm()
{
#if configUSE_TRACE_FACILITY == 1
    UBaseType_t n = uxTaskGetNumberOfTasks();
    auto* buf = static_cast<TaskStatus_t*>(
        heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        ESP_LOGW(kTag, "hwm: TaskStatus_t buffer alloc failed (n=%u)", static_cast<unsigned>(n));
        return;
    }
    n = uxTaskGetSystemState(buf, n, nullptr);
    ESP_LOGI(kTag, "=== stack HWM (bytes of headroom never used; smaller = closer to overflow) ===");
    for (UBaseType_t i = 0; i < n; ++i) {
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        const int core = static_cast<int>(buf[i].xCoreID) == INT32_MAX
                             ? -1
                             : static_cast<int>(buf[i].xCoreID);
#else
        const int core = -1;
#endif
        // ESP-IDF xtensa port: StackType_t = uint8_t → HWM unit is bytes.
        ESP_LOGI(kTag, "  %-18s core=%2d prio=%2u hwm=%6u B",
                 buf[i].pcTaskName, core,
                 static_cast<unsigned>(buf[i].uxCurrentPriority),
                 static_cast<unsigned>(buf[i].usStackHighWaterMark));
    }
    heap_caps_free(buf);
#else
    ESP_LOGW(kTag, "hwm: CONFIG_FREERTOS_USE_TRACE_FACILITY disabled — rebuild with it enabled");
#endif
}


void diag_heap_per_task()
{
#if CONFIG_HEAP_TASK_TRACKING
    // Partition 0: PSRAM. Partition 1 (caps=0/mask=0 = "everything else"):
    // internal RAM. Task stacks created with xTaskCreate are attributed to
    // the CREATING task (the allocation happens there), and everything
    // allocated before the scheduler started shows up as "Pre-Scheduler".
    constexpr std::size_t kMaxTotals = 40;
    static heap_task_totals_t totals[kMaxTotals];
    std::size_t num_totals = 0;
    heap_task_info_params_t p{};
    p.caps[0] = MALLOC_CAP_SPIRAM;
    p.mask[0] = MALLOC_CAP_SPIRAM;
    p.caps[1] = 0;
    p.mask[1] = 0;
    p.totals = totals;
    p.num_totals = &num_totals;
    p.max_totals = kMaxTotals;
    heap_caps_get_per_task_info(&p);
    ESP_LOGI(kTag, "=== heap by task (internal bytes/blocks, PSRAM bytes) ===");
    std::size_t int_sum = 0;
    for (std::size_t i = 0; i < num_totals; ++i) {
        const char* name = totals[i].task != nullptr ? pcTaskGetName(totals[i].task) : "Pre-Scheduler";
        ESP_LOGI(kTag, "  %-18s INT=%6u B /%4u  PSRAM=%8u B",
                 name, static_cast<unsigned>(totals[i].size[1]), static_cast<unsigned>(totals[i].count[1]),
                 static_cast<unsigned>(totals[i].size[0]));
        int_sum += totals[i].size[1];
    }
    ESP_LOGI(kTag, "  internal tracked total=%u B, free=%u B", static_cast<unsigned>(int_sum),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
#else
    ESP_LOGW(kTag, "per-task heap: CONFIG_HEAP_TASK_TRACKING disabled — rebuild with it enabled");
#endif
}

} // namespace stackchan::app
