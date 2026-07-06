// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "camera_service.hpp"

#include <sdkconfig.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <wifi_config_service/wifi_config_service.hpp>

#if CONFIG_STACKCHAN_CAMERA_ENABLED
#include "board/camera_gc0308.hpp"
#endif

namespace stackchan::app::camera_service {

#if CONFIG_STACKCHAN_CAMERA_ENABLED

namespace {

constexpr const char* kTag = "stackchan";

// Resident camera (M5Base only). See init_resident() in the header for the
// full lifetime rationale.
stackchan::board::CameraGc0308 g_camera;

// SharedState reference for the quiesce flag; set by register_sinks.
SharedState* g_state = nullptr;

// Quiesce the PSRAM-heavy tasks (render sprite traffic) and the In_I2C
// pollers for the duration of a camera session. The sensor streams
// continuously, so frames completed BEFORE the bus went quiet may be torn
// by concurrent PSRAM traffic — confirmed on hardware as bands of corrupt
// pixels. The quiesce also covers all runtime SCCB traffic (colorbar / RAW
// re-init / register pokes): SCCB shares GPIO12/11 with In_I2C, so the
// pollers must be parked first.
struct QuiesceGuard {
    QuiesceGuard()
    {
        g_state->i2c_quiesce.store(true, std::memory_order_release);
        // demo_loop polls every ~50 ms, led_task every 100 ms, render every
        // 33 ms — 150 ms guarantees all of them observed the flag.
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ~QuiesceGuard()
    {
        g_state->i2c_quiesce.store(false, std::memory_order_release);
    }
};

bool capture_sink(const stackchan::wifi_config::CameraCaptureOptions& options,
                  std::vector<std::uint8_t>& out, std::size_t& w, std::size_t& h,
                  std::string& format)
{
    using stackchan::board::CameraFrameSize;
    using stackchan::board::CameraPixelFormat;
    // Resident camera (g_camera, initialised at boot). Boot-time init
    // failure leaves it uninitialised — report unavailable.
    if (!g_camera.initialised()) {
        return false;
    }
    // Discard two frames after the quiesce drain so the one we keep was
    // composed entirely under quiesce.
    QuiesceGuard quiesce;

    // RAW Bayer: the DVP path is 1 B/px vs the resident RGB565's 2 B/px, so
    // this needs a driver re-init, not just a sensor register flip. Deinit →
    // BayerRaw init → grab → restore RGB565. The swap reuses the
    // internal-RAM blocks the resident driver just freed (same allocation
    // pattern), so it works at steady state where a cold init would not —
    // but a restore failure leaves the camera down until reboot, so both
    // steps log loudly.
    if (options.raw_bayer) {
        // Format switches contend with other tasks (conversation, WebSocket)
        // for fragmented internal RAM — begin() can lose the allocation race
        // (it detects this and fails loudly), so every (re)init below
        // retries a few times.
        auto begin_with_retry = [](CameraPixelFormat fmt, const char* what) -> bool {
            for (int attempt = 0; attempt < 5; ++attempt) {
                auto r = g_camera.begin(fmt, CameraFrameSize::Vga);
                if (r) {
                    return true;
                }
                if (attempt == 4) {
                    ESP_LOGE(kTag, "%s init failed: %d", what,
                             static_cast<int>(r.error()));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
            return false;
        };
        (void)g_camera.end();
        if (!begin_with_retry(CameraPixelFormat::BayerRaw, "RAW")) {
            ESP_LOGE(kTag, "restoring RGB565 after RAW init failure");
            (void)begin_with_retry(CameraPixelFormat::Rgb565, "RGB565 fallback");
            return false;
        }
        bool ok = false;
        {
            for (int i = 0; i < 2; ++i) {
                (void)g_camera.capture();
            }
            auto frame = g_camera.capture();
            if (frame) {
                out.assign(frame->data(), frame->data() + frame->size());
                w = frame->width();
                h = frame->height();
                format = "bayer8-rggb";
                ok = true;
            } else {
                ESP_LOGE(kTag, "RAW capture failed: %d",
                         static_cast<int>(frame.error()));
            }
        }
        (void)g_camera.end();
        if (!begin_with_retry(CameraPixelFormat::Rgb565, "RGB565 restore")) {
            ESP_LOGE(kTag, "camera down until reboot");
        }
        return ok;
    }

    // Colour-bar test pattern: optics-independent reference for validating
    // the transfer path. Same RGB565 pipeline, the sensor just substitutes
    // its generator for the array.
    if (options.colorbar) {
        (void)g_camera.set_colorbar(true);
    }
    format = "rgb565be";
    for (int i = 0; i < 2; ++i) {
        (void)g_camera.capture();
    }
    auto frame = g_camera.capture();
    if (!frame) {
        // The first fb_get after a long idle-streaming stretch can time out
        // (the DVP stalls once the un-consumed frame queue overflows; the
        // failed fb_get itself kicks the driver back into motion) — seen on
        // HW after the RAW swap. One more attempt recovers it.
        ESP_LOGW(kTag, "camera capture timed out — retrying once");
        frame = g_camera.capture();
    }
    if (options.colorbar) {
        (void)g_camera.set_colorbar(false);
    }
    if (!frame) {
        ESP_LOGE(kTag, "camera capture failed: %d",
                 static_cast<int>(frame.error()));
        return false;
    }
    out.assign(frame->data(), frame->data() + frame->size());
    w = frame->width();
    h = frame->height();
    return true;
}

// Raw register access for interactive colour tuning. Same quiesce rule as
// capture: SCCB may only run while the In_I2C pollers are parked.
bool reg_sink(bool write, std::uint8_t page, std::uint8_t reg, std::uint8_t& value)
{
    if (!g_camera.initialised()) {
        return false;
    }
    QuiesceGuard quiesce;
    if (write) {
        return static_cast<bool>(g_camera.write_reg(page, reg, value));
    }
    auto r = g_camera.read_reg(page, reg);
    if (!r) {
        return false;
    }
    value = *r;
    return true;
}

} // namespace

void init_resident(stackchan::board::Board& board)
{
    // VGA (full sensor): the fb size is fixed at esp_camera_init time, so
    // the resolution is a boot-time choice — 600 KiB PSRAM fb + ~4× the
    // QVGA streaming bounce bandwidth, in exchange for full-resolution
    // photos and (crucially) an intact Bayer mosaic for the RAW capture
    // path, which the QVGA subsampling would scramble.
    if (board.kind() != stackchan::board::BoardKind::M5Base) {
        return;
    }
    if (auto r = g_camera.begin(stackchan::board::CameraPixelFormat::Rgb565,
                                stackchan::board::CameraFrameSize::Vga);
        !r) {
        ESP_LOGW(kTag, "resident camera init failed: %d — photo capture disabled",
                 static_cast<int>(r.error()));
    }
}

void register_sinks(SharedState& state)
{
    if (!g_camera.initialised()) {
        return; // has_camera stays false — web UI hides the section
    }
    g_state = &state;
    stackchan::wifi_config::set_camera_capture_sink(&capture_sink);
    stackchan::wifi_config::set_camera_reg_sink(&reg_sink);
}

#else // !CONFIG_STACKCHAN_CAMERA_ENABLED

void init_resident(stackchan::board::Board& /*board*/) {}
void register_sinks(SharedState& /*state*/) {}

#endif // CONFIG_STACKCHAN_CAMERA_ENABLED

} // namespace stackchan::app::camera_service
