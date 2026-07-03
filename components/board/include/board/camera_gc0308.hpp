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
};

// Capture pixel format, selected at begin().
//   Grayscale — 1 B/px. What quirc consumes; the QR scanner uses this.
//   Rgb565    — 2 B/px big-endian (high byte first, as esp32-camera delivers
//               DVP RGB565). The settings-page photo capture uses this.
enum class CameraPixelFormat : std::uint8_t {
    Grayscale,
    Rgb565,
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
    // QVGA grayscale — 320 × 240 = 76800 bytes per frame in PSRAM.
    static constexpr std::size_t kFrameWidth = 320;
    static constexpr std::size_t kFrameHeight = 240;

    CameraGc0308() = default;
    CameraGc0308(const CameraGc0308&) = delete;
    CameraGc0308& operator=(const CameraGc0308&) = delete;
    CameraGc0308(CameraGc0308&&) = delete;
    CameraGc0308& operator=(CameraGc0308&&) = delete;
    ~CameraGc0308();

    bool initialised() const noexcept { return initialised_; }

    // Release m5::In_I2C, then call esp_camera_init with the given pixel
    // format. Idempotent: a second call while already initialised is a no-op
    // success (the original format stays — end() first to switch formats).
    //
    // Rejects with LowMemory when the internal-RAM largest contiguous block is
    // below the floor the esp32-camera driver / DMA descriptors need. Quirc's
    // image buffer also lives in PSRAM, so this is purely about the driver
    // setup itself.
    tl::expected<void, CameraError>
    begin(CameraPixelFormat format = CameraPixelFormat::Grayscale);

    // Tear down esp_camera. Camera power is always-on on the CoreS3 mainboard,
    // so no I/O expander action is taken. m5::In_I2C will auto-reinit on its
    // next access. Safe to call when not initialised (returns OK).
    tl::expected<void, CameraError> end();

    // Grab the latest QVGA grayscale frame. Returns a RAII handle whose
    // destructor returns the framebuffer to the driver; do NOT hold it across
    // a long sleep / decode loop without budgeting for the fact that the
    // driver only has one fb at a time (CAMERA_GRAB_LATEST + fb_count=1).
    tl::expected<CameraFrame, CameraError> capture();

private:
    bool initialised_ = false;
    CameraPixelFormat format_ = CameraPixelFormat::Grayscale;
};

} // namespace stackchan::board
