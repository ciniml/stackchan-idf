// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <tl/expected.hpp>

namespace stackchan::board {

// CoreS3-only GC0308 DVP camera driver. Wraps espressif/esp32-camera so the
// rest of the firmware never has to know about sensor_t / esp_camera_*.
//
// Power / reset on the CoreS3 main board:
//   - AXP2101 ALDO3 supplies 3V3 to the camera (already enabled by M5Unified
//     at boot — we do NOT touch the PMIC here).
//   - RESET and PWDN are always deasserted by pull-ups on the mainboard; no
//     AW9523 toggling is required (or performed) — this matches the M5CoreS3
//     reference firmware which also leaves AW9523 untouched for the camera.
//   - XCLK has no GPIO: the sensor clock comes from an on-board oscillator,
//     so pin_xclk = -1 in camera_config_t.
//
// I2C / SCCB sharing:
//   begin() calls m5::In_I2C.release() immediately before esp_camera_init so
//   that the SCCB driver can install its own i2c_master handle on GPIO12/11
//   without conflicting with lgfx.  lgfx reinitialises automatically on its
//   next access, so battery and LED drivers that use m5::In_I2C continue
//   to work normally during QR scanning — SCCB is nearly idle while streaming,
//   so the two are safe to share this way (same approach as the M5 reference).
//
// We never touch AXP2101 (0x34) directly or run a bus scan — both are
// forbidden by the repo-wide I2C policy (see CLAUDE.md).
//
// Not built on AtomS3R targets: the source compiles to nothing when the
// firmware doesn't include esp32-camera (e.g. atoms3r profile), and the
// `begin` / `capture` paths reject any call with `Error::NotSupported`.
// Callers can also gate on `Board::kind() == BoardKind::M5Base` to avoid
// constructing this at all on Takao base / Atom-nyan.

enum class CameraError {
    NotSupported,      // built without esp32-camera (atoms3r) or wrong board kind
    LowMemory,         // internal-RAM contiguous block < the safety floor at init time
    PowerExpander,     // reserved — no longer used (camera power is always-on on CoreS3)
    DriverInit,        // esp_camera_init returned non-OK (sensor not detected, DMA setup, ...)
    DriverDeinit,      // esp_camera_deinit returned non-OK (logged but rarely actionable)
    NotInitialised,    // capture() / deinit() called before begin()
    CaptureFailed,     // esp_camera_fb_get returned nullptr (driver timeout / no frame)
    WrongPixelFormat,  // returned frame doesn't match the begin() format (defensive)
    SensorAccess,      // SCCB register read/write failed (set_reg / colorbar / RAW switch)
    BadArgument,       // unsupported format/size combination (e.g. BayerRaw below VGA)
};

// Capture pixel format, selected at begin().
//   Grayscale — 1 B/px. What quirc consumes; the QR scanner uses this.
//   Rgb565    — 2 B/px big-endian (high byte first, as esp32-camera delivers
//               DVP RGB565). The settings-page photo capture uses this.
//   BayerRaw  — 1 B/px raw Bayer mosaic (RGGB), bypassing the sensor's ISP
//               (AWB / colour matrix / gamma / demosaic). esp32-camera's S3
//               DVP layer has no RAW pixformat, but 8-bit Bayer has the exact
//               timing of the GC0308's "only Y" grayscale mode, so we init the
//               driver as GRAYSCALE and flip the sensor's output-format
//               register (P0:0x24[4:0] = 0x17) afterwards. VGA only — the
//               driver's subsampling for smaller sizes scrambles the mosaic.
enum class CameraPixelFormat : std::uint8_t {
    Grayscale,
    Rgb565,
    BayerRaw,
};

// Capture frame size, selected at begin(). The GC0308 is a VGA sensor; QVGA
// is produced by on-sensor 1/2 subsampling (the pixel clock — and therefore
// the resident streaming bandwidth — is what changes between the two).
enum class CameraFrameSize : std::uint8_t {
    Qvga, // 320 × 240
    Vga,  // 640 × 480 (full sensor)
};

// One captured frame, owned by the esp32-camera driver. RAII: the destructor
// returns the framebuffer to the driver so it can be reused for the next
// capture. The pointer / dimensions are valid for the lifetime of this object
// and invalid afterwards.
class CameraFrame {
public:
    CameraFrame() = default;
    CameraFrame(const CameraFrame&) = delete;
    CameraFrame& operator=(const CameraFrame&) = delete;
    CameraFrame(CameraFrame&& other) noexcept;
    CameraFrame& operator=(CameraFrame&& other) noexcept;
    ~CameraFrame();

    // Pixel buffer (1 B/px grayscale or 2 B/px RGB565 big-endian, per the
    // begin() format). Lives in PSRAM (the fb_location config) — fine to
    // read from any task, but DMA-into-flash operations need a copy because
    // PSRAM is not DMA-capable.
    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t width() const noexcept { return width_; }
    std::size_t height() const noexcept { return height_; }

private:
    friend class CameraGc0308;
    // Opaque pointer to camera_fb_t* — kept void* so this header doesn't have
    // to drag in esp_camera.h. The .cpp casts it back internally.
    void* fb_handle_ = nullptr;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t width_ = 0;
    std::size_t height_ = 0;
};

class CameraGc0308 {
public:
    // Default (QVGA) frame dimensions — what begin() with CameraFrameSize::Qvga
    // yields. The QR task sizes its quirc buffer from these; frame_width() /
    // frame_height() report the live values for the size actually selected.
    static constexpr std::size_t kFrameWidth = 320;
    static constexpr std::size_t kFrameHeight = 240;

    CameraGc0308() = default;
    CameraGc0308(const CameraGc0308&) = delete;
    CameraGc0308& operator=(const CameraGc0308&) = delete;
    CameraGc0308(CameraGc0308&&) = delete;
    CameraGc0308& operator=(CameraGc0308&&) = delete;
    ~CameraGc0308();

    bool initialised() const noexcept { return initialised_; }

    // Frame dimensions for the size selected at begin() (kFrameWidth/Height
    // when not initialised).
    std::size_t frame_width() const noexcept { return expected_w_; }
    std::size_t frame_height() const noexcept { return expected_h_; }

    // Release m5::In_I2C, then call esp_camera_init with the given pixel
    // format and frame size. Idempotent: a second call while already
    // initialised is a no-op success (the original format/size stay — end()
    // first to switch).
    //
    // Rejects with LowMemory when the internal-RAM largest contiguous block is
    // below the floor the esp32-camera driver / DMA descriptors need. Quirc's
    // image buffer also lives in PSRAM, so this is purely about the driver
    // setup itself. BayerRaw at anything below Vga rejects with BadArgument.
    tl::expected<void, CameraError>
    begin(CameraPixelFormat format = CameraPixelFormat::Grayscale,
          CameraFrameSize size = CameraFrameSize::Qvga);

    // Tear down esp_camera. Camera power is always-on on the CoreS3 mainboard,
    // so no I/O expander action is taken. m5::In_I2C will auto-reinit on its
    // next access. Safe to call when not initialised (returns OK).
    tl::expected<void, CameraError> end();

    // Grab the latest frame at the begin() format/size. Returns a RAII handle
    // whose destructor returns the framebuffer to the driver; do NOT hold it
    // across a long sleep / decode loop without budgeting for the fact that
    // the driver only has one fb at a time (CAMERA_GRAB_LATEST + fb_count=1).
    tl::expected<CameraFrame, CameraError> capture();

    // Sensor-generated colour-bar test pattern on/off. Optics-independent
    // reference frames for validating the RGB565 transfer path (byte order,
    // 565 decode) end to end.
    //
    // IMPORTANT (this and the register accessors below): SCCB shares
    // GPIO12/11 with m5::In_I2C, so runtime sensor traffic must only happen
    // while the In_I2C pollers are quiesced (SharedState::i2c_quiesce) —
    // same rule as capture sessions.
    tl::expected<void, CameraError> set_colorbar(bool enable);

    // Raw SCCB register access for interactive colour tuning (AWB gains /
    // colour matrix / gamma live on pages 0-1). `page` selects via the 0xfe
    // page register; the page is restored to 0 afterwards.
    tl::expected<std::uint8_t, CameraError> read_reg(std::uint8_t page, std::uint8_t reg);
    tl::expected<void, CameraError> write_reg(std::uint8_t page, std::uint8_t reg,
                                              std::uint8_t value);

private:
    bool initialised_ = false;
    CameraPixelFormat format_ = CameraPixelFormat::Grayscale;
    std::size_t expected_w_ = kFrameWidth;
    std::size_t expected_h_ = kFrameHeight;
};

} // namespace stackchan::board
