// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "qr_task.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include "board/camera_gc0308.hpp"

#if CONFIG_STACKCHAN_CAMERA_ENABLED
#include <quirc.h>
#endif

namespace stackchan::app {

namespace {

constexpr const char* kTag = "qr";

// Stack budget for the worker. Verified against the vendored quirc 1.2
// sources (managed_components/espressif__quirc/quirc/lib/identify.c):
// flood_fill_seed uses an EXPLICIT stack stored inside the quirc buffer
// (q->flood_fill_vars, PSRAM here) — there is no C-stack recursion anywhere
// in the decode path, so the worker only needs frames for esp_camera
// capture + quirc API calls + logging. 5 KiB fits inside the steady-state
// internal-RAM largest block (~7.4 KiB with the conversation live); 8 KiB
// did not. PSRAM stack is forbidden by the project rule (cache pollution +
// stalls against the prio-6 audio tasks).
constexpr std::uint32_t kTaskStackBytes = 5 * 1024;

// Per-payload de-dupe window. The user typically holds the QR in front of
// the lens for ~1 s, which yields ~5-10 successful decodes of the same
// string at QVGA. Logging each one floods the serial console and would
// later (Phase 4) trigger multiple BLE notifies for one physical scan.
constexpr std::uint32_t kSuppressMs = 3000;

// Worker control. start_qr_scan owns lifecycle; stop sets stop_requested
// and waits (loosely) for the worker to ack.
std::atomic<bool> g_stop_requested{false};
std::atomic<TaskHandle_t> g_task_handle{nullptr};

#if CONFIG_STACKCHAN_CAMERA_ENABLED

// Per-loop housekeeping helper: copy the camera grayscale buffer into the
// quirc input buffer. quirc owns its own image buffer (allocated inside
// quirc_resize); we feed it via quirc_begin which returns a pointer to that
// internal buffer. esp_camera's frame buffer lives in PSRAM (CAMERA_FB_IN_
// PSRAM) so the memcpy crosses PSRAM↔PSRAM if quirc's buffer was placed
// there too — a non-DMA, cache-only copy that the CPU can run from either
// core without contention.
bool feed_quirc(quirc* q, const board::CameraFrame& frame)
{
    int qw = 0, qh = 0;
    std::uint8_t* dst = quirc_begin(q, &qw, &qh);
    if (dst == nullptr) {
        ESP_LOGW(kTag, "quirc_begin returned null");
        return false;
    }
    if (qw != static_cast<int>(frame.width()) ||
        qh != static_cast<int>(frame.height())) {
        // The dimensions were set by quirc_resize at task start; a mismatch
        // here would mean someone changed the camera config midway. Treat
        // as a hard error so we don't memcpy with the wrong stride.
        ESP_LOGE(kTag, "quirc size mismatch (q=%dx%d, frame=%ux%u)",
                 qw, qh, static_cast<unsigned>(frame.width()),
                 static_cast<unsigned>(frame.height()));
        return false;
    }
    std::memcpy(dst, frame.data(), frame.size());
    quirc_end(q);
    return true;
}

struct WorkerArgs {
    board::Board* board;
};

void qr_worker(void* arg)
{
    std::unique_ptr<WorkerArgs> args{static_cast<WorkerArgs*>(arg)};
    if (args == nullptr || args->board == nullptr) {
        ESP_LOGE(kTag, "worker started with null args");
        g_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    // Board reference held only to keep ownership intent clear (and to leave
    // room for Phase 3 board-state queries); the camera power path runs
    // through CameraGc0308 / m5::In_I2C directly.
    (void)args->board;

    // Camera power-up sequence per CLAUDE.md: ALDO3 is already on (M5Unified
    // brought it up at boot); CameraGc0308::begin() drives RESET via the
    // AW9523 and then runs esp_camera_init. The 12 KiB internal-RAM floor
    // check lives inside begin().
    board::CameraGc0308 camera;
    if (auto r = camera.begin(); !r) {
        ESP_LOGE(kTag, "camera.begin() failed: %d", static_cast<int>(r.error()));
        g_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    // Quirc setup. quirc_new + quirc_resize do internal allocations; with
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024 in our sdkconfig.defaults the
    // ~76 KiB image buffer naturally falls into PSRAM (no special path
    // needed — every alloc above 1 KiB prefers PSRAM by default).
    quirc* q = quirc_new();
    if (q == nullptr) {
        ESP_LOGE(kTag, "quirc_new returned null");
        (void)camera.end();
        g_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    if (quirc_resize(q, board::CameraGc0308::kFrameWidth,
                     board::CameraGc0308::kFrameHeight) < 0) {
        ESP_LOGE(kTag, "quirc_resize failed (likely OOM in PSRAM)");
        quirc_destroy(q);
        (void)camera.end();
        g_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "scan started — PSRAM largest=%u, INT largest=%u",
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    std::string last_payload;
    std::uint32_t last_payload_ms = 0;

    while (!g_stop_requested.load(std::memory_order_acquire)) {
        auto frame_r = camera.capture();
        if (!frame_r) {
            // Transient capture failures are normal during startup while the
            // sensor's AGC settles; ramp the log severity by frequency rather
            // than per-frame so the console isn't drowned.
            static int s_warn_throttle = 0;
            if ((s_warn_throttle++ & 31) == 0) {
                ESP_LOGW(kTag, "capture failed: %d", static_cast<int>(frame_r.error()));
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!feed_quirc(q, *frame_r)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const int count = quirc_count(q);
        for (int i = 0; i < count; ++i) {
            quirc_code code;
            quirc_data data;
            quirc_extract(q, i, &code);
            const quirc_decode_error_t derr = quirc_decode(&code, &data);
            if (derr != QUIRC_SUCCESS) {
                // Most frames in a real scan session have a candidate that
                // fails ECC during early focus / motion; log at DEBUG only.
                ESP_LOGD(kTag, "decode error: %s", quirc_strerror(derr));
                continue;
            }
            // payload is NOT null-terminated; bound by data.payload_len.
            // 3-second suppression: stamp the exact byte sequence so URLs
            // that differ only at the tail still log distinctly.
            std::string payload(reinterpret_cast<const char*>(data.payload),
                                static_cast<std::size_t>(data.payload_len));
            const std::uint32_t now_ms =
                static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
            if (payload == last_payload && (now_ms - last_payload_ms) < kSuppressMs) {
                continue;
            }
            last_payload = payload;
            last_payload_ms = now_ms;
            // Phase 3/4 marker: this is the single producer line that will
            // grow a device_ui sink (Phase 3) and a BLE notify (Phase 4).
            // Keep the ESP_LOGI here even after those land so the serial
            // trace stays the canonical source of truth for "did we
            // physically decode this QR".
            ESP_LOGI(kTag, "decoded: %s", payload.c_str());
        }

        // Frame buffer is released by ~CameraFrame as it goes out of scope
        // here, returning to the driver for the next CAMERA_GRAB_LATEST.
        // Yield so the camera driver task can run; without this the worker
        // monopolises CPU 0 between captures.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(kTag, "scan stopping — cleaning up");
    quirc_destroy(q);
    (void)camera.end();

    g_stop_requested.store(false, std::memory_order_release);
    g_task_handle.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}

#endif // CONFIG_STACKCHAN_CAMERA_ENABLED

} // namespace

bool start_qr_scan(board::Board& board)
{
#if !CONFIG_STACKCHAN_CAMERA_ENABLED
    (void)board;
    ESP_LOGW(kTag, "start_qr_scan: camera support not compiled in");
    return false;
#else
    // Board-kind gate. Only the M5 CoreS3 base wires the GC0308 in; on
    // Takao base / Atom-nyan the camera doesn't exist and the SCCB probe
    // would NACK forever.
    if (!stackchan::board::profile_for(board.kind()).has_camera) {
        ESP_LOGW(kTag, "start_qr_scan: board kind %d has no GC0308 — skipping",
                 static_cast<int>(board.kind()));
        return false;
    }

    if (g_task_handle.load(std::memory_order_acquire) != nullptr) {
        ESP_LOGW(kTag, "start_qr_scan: already running");
        return false;
    }

    auto* args = new WorkerArgs{&board};

    TaskHandle_t handle = nullptr;
    // Pin to CPU 0. CPU 1 already carries render (prio 5), servo (prio 4),
    // M5 speaker / mic (prio 6); adding the QR worker there would starve
    // render and trip the task watchdog (the same failure mode that drove
    // mcp_say to CPU 0 — see commit fc23e8d). CPU 0 has NimBLE + Wi-Fi event
    // tasks but plenty of headroom between event bursts.
    const BaseType_t rc = xTaskCreatePinnedToCore(
        qr_worker, "qr_task", kTaskStackBytes, args,
        tskIDLE_PRIORITY + 2, &handle, 0);
    if (rc != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate failed: %d (INT largest=%u)",
                 static_cast<int>(rc),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        delete args;
        return false;
    }
    g_task_handle.store(handle, std::memory_order_release);
    return true;
#endif
}

bool qr_scan_active()
{
    return g_task_handle.load(std::memory_order_acquire) != nullptr;
}

void stop_qr_scan()
{
    if (g_task_handle.load(std::memory_order_acquire) == nullptr) {
        return;
    }
    g_stop_requested.store(true, std::memory_order_release);
    // Don't block here — the caller (potentially device_ui in Phase 3) is
    // running in a UI / HTTP context where a 100-200 ms wait would be felt.
    // The worker checks the flag every frame and self-deletes within a few
    // hundred ms; g_task_handle is cleared by the worker before vTaskDelete.
}

} // namespace stackchan::app
