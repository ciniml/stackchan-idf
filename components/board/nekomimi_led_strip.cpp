// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/nekomimi_led_strip.hpp"

#include <esp_log.h>
#include <led_strip.h>
#include <led_strip_rmt.h>

namespace stackchan::board {

namespace {

constexpr const char* kTag = "nekomimi-led";

// 10 MHz RMT resolution → 100 ns per tick, which is enough to express the
// WS2812 ~350 ns timing buckets cleanly. The driver's symbol generator handles
// the actual 800 kHz bit cadence.
constexpr std::uint32_t kRmtResolutionHz = 10'000'000;

} // namespace

NekomimiLedStrip::~NekomimiLedStrip() noexcept
{
    if (handle_ != nullptr) {
        led_strip_del(handle_);
        handle_ = nullptr;
    }
}

tl::expected<void, Error> NekomimiLedStrip::begin()
{
    if (handle_ != nullptr) return {}; // idempotent

    led_strip_config_t strip_cfg{};
    strip_cfg.strip_gpio_num = kDataGpio;
    strip_cfg.max_leds = static_cast<std::uint32_t>(kCount);
    strip_cfg.led_model = LED_MODEL_WS2812;
    // WS2812 wire order is GRB; the driver writes set_pixel(r,g,b) for us so
    // we don't have to swizzle ourselves — keep the local buf_ in GRB order
    // only to size the array, not for ordering.
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_cfg.flags.invert_out = 0;

    led_strip_rmt_config_t rmt_cfg{};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = kRmtResolutionHz;
    // Single mem block (48 symbols) is plenty for 18 LEDs × 24 bits = 432
    // symbols streamed in DMA-less mode (driver chunks internally).
    rmt_cfg.mem_block_symbols = 48;
    rmt_cfg.flags.with_dma = 0;

    if (const esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &handle_); err != ESP_OK) {
        ESP_LOGE(kTag, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        handle_ = nullptr;
        return tl::unexpected{Error::LedStripIo};
    }
    clear();
    return show();
}

void NekomimiLedStrip::clear() noexcept
{
    buf_.fill(0);
}

void NekomimiLedStrip::fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    for (std::size_t i = 0; i < kCount; ++i) {
        // Store in GRB order so show() can blit straight; the driver's
        // GRB color-component fmt expects (g, r, b) byte triples on the
        // pixel array, but we drive via led_strip_set_pixel which takes
        // separate r/g/b args — order in buf_ is just storage.
        buf_[i * 3 + 0] = g;
        buf_[i * 3 + 1] = r;
        buf_[i * 3 + 2] = b;
    }
}

void NekomimiLedStrip::set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    if (index >= kCount) return;
    buf_[index * 3 + 0] = g;
    buf_[index * 3 + 1] = r;
    buf_[index * 3 + 2] = b;
}

tl::expected<void, Error> NekomimiLedStrip::show()
{
    if (handle_ == nullptr) return tl::unexpected{Error::LedStripIo};
    for (std::size_t i = 0; i < kCount; ++i) {
        const std::uint8_t g = buf_[i * 3 + 0];
        const std::uint8_t r = buf_[i * 3 + 1];
        const std::uint8_t b = buf_[i * 3 + 2];
        if (const esp_err_t err = led_strip_set_pixel(handle_, static_cast<std::uint32_t>(i), r, g, b);
            err != ESP_OK) {
            ESP_LOGW(kTag, "led_strip_set_pixel(%u) failed: %s",
                     static_cast<unsigned>(i), esp_err_to_name(err));
            return tl::unexpected{Error::LedStripIo};
        }
    }
    if (const esp_err_t err = led_strip_refresh(handle_); err != ESP_OK) {
        ESP_LOGW(kTag, "led_strip_refresh failed: %s", esp_err_to_name(err));
        return tl::unexpected{Error::LedStripIo};
    }
    return {};
}

} // namespace stackchan::board
