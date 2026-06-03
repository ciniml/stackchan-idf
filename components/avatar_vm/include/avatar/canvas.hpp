// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>

#include <M5GFX.h> // lgfx::IFont / lgfx::textdatum_t (real header, or the WASM shim)

namespace stackchan::avatar {

// Abstract drawing surface the avatar renders through. Method names mirror
// LovyanGFX so the element draw code reads the same regardless of strategy.
//
// Two concrete strategies implement this (see canvas_m5gfx.hpp):
//   - BufferedCanvas: composes the whole frame into one full-screen sprite and
//     pushes it once (needs PSRAM). Today's behaviour.
//   - DirectCanvas:   draws cheap primitives straight to the panel and composes
//     multi-primitive elements via a small bounding-box scratch sprite (no
//     full-screen buffer — for PSRAM-less devices).
//
// This base declares only the primitives the *face* code uses, so it stays
// satisfiable by the Emscripten/WASM shim (which is not a LovyanGFX). The
// richer text/round-rect surface used by the balloon / UI / gauge is RichCanvas
// below (firmware only). Coordinates are int32 to match LovyanGFX; colours are
// RGB565.
class Canvas {
public:
    virtual ~Canvas() = default;

    virtual std::int32_t width() const = 0;
    virtual std::int32_t height() const = 0;

    virtual void fillScreen(std::uint16_t color) = 0;
    virtual void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                          std::uint16_t color) = 0;
    virtual void fillCircle(std::int32_t x, std::int32_t y, std::int32_t r, std::uint16_t color) = 0;
    virtual void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                              std::int32_t x2, std::int32_t y2, std::uint16_t color) = 0;

    // --- Rendering-strategy hooks (no-ops for the buffered strategy) --------
    // Group a multi-primitive element so the direct strategy can composite it
    // off-screen into the bounding box (x,y,w,h) and blit once (no flicker /
    // partial draws). Calls between begin/end use absolute coordinates.
    virtual void begin_group(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) = 0;
    virtual void end_group() = 0;

    // Frame boundaries. begin_frame clears the background (buffered: fillScreen
    // the whole sprite; direct: only on a pending full repaint). end_frame
    // presents (buffered: pushSprite; direct: no-op — already on the panel).
    virtual void begin_frame(std::uint16_t bg) = 0;
    virtual void end_frame() = 0;
    // Direct strategy: force a full-screen clear + repaint on the next frame
    // (e.g. expression / layout change, or returning from the on-device UI).
    // No-op for the buffered strategy (it clears every frame anyway).
    virtual void request_full_repaint() = 0;
};

// Adds the text / rounded-rect / colour helpers used by the balloon, the
// on-device UI and the battery gauge. Firmware only — not used by the WASM
// preview (which never compiles balloon.cpp / device_ui.cpp), so the shim need
// not satisfy it.
class RichCanvas : public Canvas {
public:
    virtual std::uint16_t color565(std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;

    virtual void fillRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                               std::int32_t r, std::uint16_t color) = 0;
    virtual void drawRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                               std::int32_t r, std::uint16_t color) = 0;

    virtual void setTextColor(std::uint16_t fg) = 0;
    virtual void setTextColor(std::uint16_t fg, std::uint16_t bg) = 0;
    virtual void setFont(const lgfx::IFont* font) = 0;
    virtual void setTextSize(float size) = 0;
    virtual void setTextDatum(lgfx::textdatum_t datum) = 0;
    virtual void drawString(const char* str, std::int32_t x, std::int32_t y) = 0;
    virtual std::int32_t textWidth(const char* str) = 0;
    virtual void setClipRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) = 0;
    virtual void clearClipRect() = 0;
};

} // namespace stackchan::avatar
