// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

namespace stackchan::app {

// SRAM-shortage diagnostics. All read-only / log-only — no behaviour change.
//
// Register the heap alloc-failure hook (logs requested size + caps + caller +
// remaining heap whenever ANY heap_caps alloc fails — this is how we catch
// the esp-aes "Failed to allocate memory" with its actual request size).
// Call once, as early as possible in app_main.
void diag_register_alloc_fail_hook();

// One line of heap state: internal free / largest / min-ever plus the
// DMA-capable largest block (the metric esp-aes actually depends on).
void diag_heap(const char* label);

// Dump the stack high-water-mark of every task. ESP-IDF's xtensa port
// defines StackType_t as uint8_t, so the HWM unit is BYTES (the smallest
// remaining headroom the task has ever had). Use this to right-size task
// stacks from measurements instead of guesses. Requires
// CONFIG_FREERTOS_USE_TRACE_FACILITY=y.
void diag_stack_hwm();

// Dump live heap usage per task (internal bytes/blocks and PSRAM bytes) so
// the internal-RAM budget can be attributed. Requires
// CONFIG_HEAP_TASK_TRACKING=y (diagnostic builds only; it costs a few bytes
// per block); logs a warning otherwise.
void diag_heap_per_task();

} // namespace stackchan::app
