// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/camera_gc0308.hpp"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#if CONFIG_STACKCHAN_CAMERA_ENABLED
#include <M5Unified.h>
#include <esp_camera.h>
#endif

namespace stackchan::board {

namespace {

constexpr const char* kTag = "camera";

#if CONFIG_STACKCHAN_CAMERA_ENABLED

// Internal-RAM contiguous-block floor before we'll even try esp_camera_init.
// The driver allocates a handful of DMA descriptors + LCD_CAM peripheral
// state in internal RAM; coming in below this floor either fails the init
// outright or fragments what's left so the next TLS handshake can't allocate
// its AES bounce. 12 KiB matches the project-wide policy in CLAUDE.md.
constexpr std::size_t kMinInternalLargestBytes = 12 * 1024;

// GC0308 XCLK is wired directly on the CoreS3 mainboard — no GPIO needed.
// The sensor generates its pixel clock internally; pin_xclk = -1 tells
// esp32-camera not to touch any LEDC output pin.  LEDC_TIMER_0/CHANNEL_0
// are still required struct fields but are inert when pin_xclk < 0.
constexpr int kXclkFreqHz = 20'000'000; // matches M5CoreS3 reference firmware

#endif // CONFIG_STACKCHAN_CAMERA_ENABLED

} // namespace

// --- CameraFrame --------------------------------------------------------

CameraFrame::CameraFrame(CameraFrame&& other) noexcept
    : fb_handle_{other.fb_handle_}, data_{other.data_}, size_{other.size_},
      width_{other.width_}, height_{other.height_}
{
    other.fb_handle_ = nullptr;
    other.data_ = nullptr;
    other.size_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

CameraFrame& CameraFrame::operator=(CameraFrame&& other) noexcept
{
    if (this != &other) {
        // Release any framebuffer we currently hold before taking the new one.
        // (Self-move is guarded above.)
        this->~CameraFrame();
        new (this) CameraFrame(std::move(other));
    }
    return *this;
}

CameraFrame::~CameraFrame()
{
#if CONFIG_STACKCHAN_CAMERA_ENABLED
    if (fb_handle_ != nullptr) {
        esp_camera_fb_return(static_cast<camera_fb_t*>(fb_handle_));
        fb_handle_ = nullptr;
    }
#endif
}

// --- CameraGc0308 --------------------------------------------------------

CameraGc0308::~CameraGc0308()
{
    // Best-effort teardown if the user forgot. Ignore the result — we're
    // already in a destructor and there's nothing useful to do with a failure.
    (void)end();
}

#if CONFIG_STACKCHAN_CAMERA_ENABLED

tl::expected<void, CameraError> CameraGc0308::begin(CameraPixelFormat format, CameraFrameSize size)
{
    if (initialised_) {
        return {}; // idempotent (keeps the original format/size)
    }
    // BayerRaw needs the full sensor array: the driver's on-sensor
    // subsampling for smaller sizes averages/skips pixels and scrambles the
    // Bayer mosaic beyond recovery.
    if (format == CameraPixelFormat::BayerRaw && size != CameraFrameSize::Vga) {
        return tl::unexpected{CameraError::BadArgument};
    }

    // Guard against the steady-state internal-RAM shortage. esp_camera_init
    // does internal allocations for the LCD_CAM peripheral state + a small
    // DMA descriptor chain (the frame buffer itself lives in PSRAM thanks to
    // CAMERA_FB_IN_PSRAM, so we only need internal RAM for these few KiB).
    const std::size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (internal_largest < kMinInternalLargestBytes) {
        ESP_LOGE(kTag, "begin: internal-RAM largest=%u below floor %u — refusing init",
                 static_cast<unsigned>(internal_largest),
                 static_cast<unsigned>(kMinInternalLargestBytes));
        return tl::unexpected{CameraError::LowMemory};
    }

    // Release M5Unified's lgfx I2C driver before calling esp_camera_init.
    // Both share GPIO12 (SIOD) and GPIO11 (SIOC); the SCCB driver inside
    // esp32-camera installs its own i2c_master handle on those pins.
    // lgfx::I2C auto-reinitialises on its next access, so no matching
    // release is needed in end(). This mirrors the M5CoreS3 reference
    // begin() which calls M5.In_I2C.release() for the same reason.
    //
    // Note: during QR scanning, battery/LED components continue to use
    // m5::In_I2C on other I2C peripherals; SCCB is nearly idle while
    // streaming, so sharing the bus this way is safe — same approach as
    // the M5 reference firmware.
    m5::In_I2C.release();

    camera_config_t cfg = {};
    cfg.pin_pwdn  = -1;   // PWDN tied to GND on the CoreS3 mainboard — not driven.
    cfg.pin_reset = -1;   // RESET is always-high (no AW9523 control needed, matches
                          // M5CoreS3 reference firmware which does not touch AW9523).
    cfg.pin_xclk  = -1;   // XCLK has no GPIO on the CoreS3 — sensor clock is
                          // supplied by a dedicated oscillator on the mainboard.
    cfg.pin_sccb_sda = CONFIG_STACKCHAN_CAMERA_PIN_SIOD;
    cfg.pin_sccb_scl = CONFIG_STACKCHAN_CAMERA_PIN_SIOC;
    cfg.pin_d7 = CONFIG_STACKCHAN_CAMERA_PIN_D7;
    cfg.pin_d6 = CONFIG_STACKCHAN_CAMERA_PIN_D6;
    cfg.pin_d5 = CONFIG_STACKCHAN_CAMERA_PIN_D5;
    cfg.pin_d4 = CONFIG_STACKCHAN_CAMERA_PIN_D4;
    cfg.pin_d3 = CONFIG_STACKCHAN_CAMERA_PIN_D3;
    cfg.pin_d2 = CONFIG_STACKCHAN_CAMERA_PIN_D2;
    cfg.pin_d1 = CONFIG_STACKCHAN_CAMERA_PIN_D1;
    cfg.pin_d0 = CONFIG_STACKCHAN_CAMERA_PIN_D0;
    cfg.pin_vsync = CONFIG_STACKCHAN_CAMERA_PIN_VSYNC;
    cfg.pin_href  = CONFIG_STACKCHAN_CAMERA_PIN_HREF;
    cfg.pin_pclk  = CONFIG_STACKCHAN_CAMERA_PIN_PCLK;

    cfg.xclk_freq_hz = kXclkFreqHz;  // required field; value is inert (pin_xclk=-1)
    cfg.ledc_timer   = LEDC_TIMER_0;   // required fields; inert when pin_xclk < 0
    cfg.ledc_channel = LEDC_CHANNEL_0;

    cfg.sccb_i2c_port = -1; // let esp32-camera manage its own i2c_master port

    // Caller's format at the caller's size. Grayscale (1 B/px) is all the QR
    // decoder needs; RGB565 (2 B/px, big-endian on the DVP path) serves the
    // settings-page colour photo; BayerRaw inits the driver as GRAYSCALE
    // (identical 1 B/px DVP timing — GC0308 gets the plain-memcpy Y8 path)
    // and switches the sensor's output format to the raw mosaic below. The
    // frame buffer lives in PSRAM (VGA RGB565 = 600 KiB) so none of this
    // touches internal RAM beyond the driver's DMA bounce.
    cfg.pixel_format = (format == CameraPixelFormat::Rgb565)
                           ? PIXFORMAT_RGB565
                           : PIXFORMAT_GRAYSCALE;
    cfg.frame_size   = (size == CameraFrameSize::Vga) ? FRAMESIZE_VGA : FRAMESIZE_QVGA;

    cfg.jpeg_quality = 0;            // unused at GRAYSCALE / RGB565
    cfg.fb_count     = 1;            // one PSRAM-resident frame — see grab_mode
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    // GRAB_LATEST throws away in-flight frames if the consumer falls behind,
    // which keeps quirc reading freshly-aimed-at frames instead of a stale
    // queue from before the user lifted the QR towards the lens.
    cfg.grab_mode = CAMERA_GRAB_LATEST;

    const esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_camera_init failed: 0x%x", static_cast<unsigned>(err));
        return tl::unexpected{CameraError::DriverInit};
    }

    if (format == CameraPixelFormat::BayerRaw) {
        // Flip the sensor to raw Bayer output, bypassing the on-chip ISP
        // (AWB / colour matrix / gamma / demosaic). P0:0x24[4:0] = 0x17
        // outputs the mosaic with the pattern selected by P1:0x53[6:5];
        // we pin that to 01 = RGGB so the host-side demosaic doesn't have
        // to guess. AEC still runs (exposure is array-level, not ISP).
        sensor_t* s = esp_camera_sensor_get();
        int ret = (s == nullptr) ? -1 : 0;
        if (ret == 0) ret = s->set_reg(s, 0xfe, 0xff, 0x01);
        if (ret == 0) ret = s->set_reg(s, 0x53, 0x60, 0x20); // Bayer pattern = RGGB
        if (ret == 0) ret = s->set_reg(s, 0xfe, 0xff, 0x00);
        if (ret == 0) ret = s->set_reg(s, 0x24, 0x1f, 0x17); // output = raw Bayer
        if (ret != 0) {
            ESP_LOGE(kTag, "BayerRaw sensor switch failed (%d)", ret);
            (void)esp_camera_deinit();
            return tl::unexpected{CameraError::SensorAccess};
        }
    }

    initialised_ = true;
    format_ = format;
    expected_w_ = (size == CameraFrameSize::Vga) ? 640 : kFrameWidth;
    expected_h_ = (size == CameraFrameSize::Vga) ? 480 : kFrameHeight;
    ESP_LOGI(kTag, "GC0308 init OK (%ux%u %s, pin_xclk=-1, internal-largest=%u)",
             static_cast<unsigned>(expected_w_), static_cast<unsigned>(expected_h_),
             format == CameraPixelFormat::Rgb565      ? "RGB565"
             : format == CameraPixelFormat::BayerRaw  ? "Bayer RAW"
                                                      : "grayscale",
             static_cast<unsigned>(internal_largest));
    return {};
}

tl::expected<void, CameraError> CameraGc0308::end()
{
    if (!initialised_) {
        return {};
    }
    const esp_err_t err = esp_camera_deinit();
    initialised_ = false;
    // No AW9523 reset to reassert — camera power is always-on on this board.
    // lgfx::I2C (m5::In_I2C) will reinitialise automatically on its next
    // access after our earlier release() call, so no action needed here.
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_camera_deinit returned 0x%x", static_cast<unsigned>(err));
        return tl::unexpected{CameraError::DriverDeinit};
    }
    return {};
}

tl::expected<CameraFrame, CameraError> CameraGc0308::capture()
{
    if (!initialised_) {
        return tl::unexpected{CameraError::NotInitialised};
    }
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
        return tl::unexpected{CameraError::CaptureFailed};
    }
    // BayerRaw rides the driver's GRAYSCALE path (see begin()); the driver
    // reports the fb as grayscale even though the bytes are the raw mosaic.
    const pixformat_t expected = (format_ == CameraPixelFormat::Rgb565)
                                     ? PIXFORMAT_RGB565
                                     : PIXFORMAT_GRAYSCALE;
    if (fb->format != expected ||
        fb->width != expected_w_ || fb->height != expected_h_) {
        esp_camera_fb_return(fb);
        return tl::unexpected{CameraError::WrongPixelFormat};
    }
    CameraFrame frame;
    frame.fb_handle_ = fb;
    frame.data_ = fb->buf;
    frame.size_ = fb->len;
    frame.width_ = fb->width;
    frame.height_ = fb->height;
    return frame;
}

tl::expected<void, CameraError> CameraGc0308::set_colorbar(bool enable)
{
    if (!initialised_) {
        return tl::unexpected{CameraError::NotInitialised};
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s == nullptr || s->set_colorbar(s, enable ? 1 : 0) != 0) {
        return tl::unexpected{CameraError::SensorAccess};
    }
    return {};
}

tl::expected<std::uint8_t, CameraError> CameraGc0308::read_reg(std::uint8_t page, std::uint8_t reg)
{
    if (!initialised_) {
        return tl::unexpected{CameraError::NotInitialised};
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s == nullptr) {
        return tl::unexpected{CameraError::SensorAccess};
    }
    // Page select (0xfe), read, restore page 0. get_reg returns the masked
    // value or <0 on bus error — but 0 is also a valid register value, so
    // only the page writes can be checked reliably.
    if (s->set_reg(s, 0xfe, 0xff, page) != 0) {
        return tl::unexpected{CameraError::SensorAccess};
    }
    const int v = s->get_reg(s, reg, 0xff);
    (void)s->set_reg(s, 0xfe, 0xff, 0x00);
    if (v < 0) {
        return tl::unexpected{CameraError::SensorAccess};
    }
    return static_cast<std::uint8_t>(v);
}

tl::expected<void, CameraError> CameraGc0308::write_reg(std::uint8_t page, std::uint8_t reg,
                                                        std::uint8_t value)
{
    if (!initialised_) {
        return tl::unexpected{CameraError::NotInitialised};
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s == nullptr) {
        return tl::unexpected{CameraError::SensorAccess};
    }
    int ret = s->set_reg(s, 0xfe, 0xff, page);
    if (ret == 0) ret = s->set_reg(s, reg, 0xff, value);
    (void)s->set_reg(s, 0xfe, 0xff, 0x00);
    if (ret != 0) {
        return tl::unexpected{CameraError::SensorAccess};
    }
    return {};
}

#else // !CONFIG_STACKCHAN_CAMERA_ENABLED

// Stub implementation for board variants that don't compile esp32-camera
// (atoms3r). All entry points reject so the rest of the firmware can still
// link / spin up the QR task; whoever called begin() handles the
// NotSupported error and skips spawning the worker.

tl::expected<void, CameraError> CameraGc0308::begin(CameraPixelFormat /*format*/,
                                                    CameraFrameSize /*size*/)
{
    ESP_LOGW(kTag, "begin: camera support not compiled in (CONFIG_STACKCHAN_CAMERA_ENABLED=n)");
    return tl::unexpected{CameraError::NotSupported};
}

tl::expected<void, CameraError> CameraGc0308::end()
{
    return {}; // nothing to do
}

tl::expected<CameraFrame, CameraError> CameraGc0308::capture()
{
    return tl::unexpected{CameraError::NotSupported};
}

tl::expected<void, CameraError> CameraGc0308::set_colorbar(bool /*enable*/)
{
    return tl::unexpected{CameraError::NotSupported};
}

tl::expected<std::uint8_t, CameraError> CameraGc0308::read_reg(std::uint8_t /*page*/,
                                                               std::uint8_t /*reg*/)
{
    return tl::unexpected{CameraError::NotSupported};
}

tl::expected<void, CameraError> CameraGc0308::write_reg(std::uint8_t /*page*/, std::uint8_t /*reg*/,
                                                        std::uint8_t /*value*/)
{
    return tl::unexpected{CameraError::NotSupported};
}

#endif // CONFIG_STACKCHAN_CAMERA_ENABLED

} // namespace stackchan::board
