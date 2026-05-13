# M5GFX / M5Unified — ESP-IDF 6.0 patches

`components/M5GFX` (0.2.20) and `components/M5Unified` (0.2.14) are vendored as
git submodules pinned to upstream `master`. Both libraries target ESP-IDF 5.x
and need a handful of patches to build and run on ESP-IDF 6.0 / ESP32-S3
(CoreS3 + Stack-chan). Patches live under [patches/](patches/) and are applied
in-place to the submodule worktrees by [tools/apply-m5-patches.sh](tools/apply-m5-patches.sh).

After applying, the submodules show as dirty in `git status` — this is
expected. Until a fork is published, treat these as a working fork.

## Workflow

```sh
git submodule update --init --recursive   # clone submodules at pinned commits
tools/apply-m5-patches.sh                  # apply local patches in-place
```

To re-sync from upstream:

```sh
git -C components/M5GFX     reset --hard
git -C components/M5Unified reset --hard
tools/apply-m5-patches.sh
```

## What the patches change

### IDF 6.0 build fixes (originated from `tab5_claude_client`)

- **M5GFX `CMakeLists.txt`** — adds an `IDF_VERSION_MAJOR GREATER_EQUAL 6`
  branch listing the per-peripheral driver components that IDF 6 split out of
  the umbrella `driver` component: `esp_driver_ledc`, `esp_driver_i2s`,
  `esp_driver_gpio`, `esp_driver_spi`, `esp_driver_i2c`, plus `esp_psram` and
  `esp_rom`.
- **M5GFX `Bus_Parallel8.hpp`** — injects a `typedef int i2s_port_t;` shim
  under IDF 6 so the header's `config_t::i2s_port` field still compiles.
- **M5GFX `Panel_DSI.cpp`** — drops the call to the removed
  `esp_lcd_dpi_panel_enable_dma2d()` and instead sets
  `dpi_config.flags.use_dma2d = true` before `esp_lcd_new_panel_dpi()`.
- **M5Unified `CMakeLists.txt`** — same per-peripheral driver REQUIRES list as
  M5GFX.
- **M5Unified `Speaker_Class.{hpp,cpp}` / `Mic_Class.{hpp,cpp}`** — shims
  the removed `i2s_port_t` enum and `SOC_I2S_NUM` / `I2S_NUM_MAX` macros.
  Per-target SOC_I2S_NUM is hard-coded based on `CONFIG_IDF_TARGET_*`
  (ESP32-S3 = 2; ESP32 = 2; ESP32-P4 = 3; others = 1). Struct field
  initialisers go through `(i2s_port_t)I2S_NUM_x` so they parse under both
  the IDF 5 enum and the IDF 6 typedef-int.

### CoreS3 LCD support on IDF 6.0 — from [M5GFX issue #192](https://github.com/m5stack/M5GFX/issues/192)

The CoreS3 ILI9342C panel never lights up under IDF 6.0 even though
backlight and I²C come up cleanly. The patch is sourced from
[wheelbot-tech/M5GFX `idfv6.0`](https://github.com/wheelbot-tech/M5GFX/tree/idfv6.0)
and addresses three root causes:

- **LCD RST is on AW9523 P1.1, not P1.5** as upstream assumes. The
  `Panel_M5StackCoreS3::rst_control` override now writes the correct bit.
- **Direct SPI register writes (`*spi_cmd_reg = SPI_EXECUTE`) no longer
  reliably clock data out on ESP32-S3 + IDF 6.0.** The patch defines
  `LGFX_SPI_USE_IDF_POLLING_TRANSMIT` for that target and routes
  `Bus_SPI::{writeCommand,writeData,writeDataRepeat,writeBytes}` through
  `spi_device_polling_transmit()` instead. A new `lgfx::v1::spi::transmit()`
  helper wraps it.
- **`spi_bus_initialize()` connects GPIO 35 to a peripheral output via the
  GPIO matrix, silently overriding D/C control.** `cs_control` now resets
  `GPIO_FUNC35_OUT_SEL_CFG_REG` to `SIG_GPIO_OUT_IDX` on every CS edge so
  the simple-GPIO output path drives the pin.

Adjacent changes from the same patch:

- Autodetect supplies an explicit hardware reset pulse via AW9523 P1.1
  (LOW 20 ms → HIGH 120 ms) before SPI panel setup.
- `bus_cfg.pin_miso = GPIO_NUM_NC` (CoreS3 hardware doesn't share MISO with
  DC), `spi_3wire = false`, `spi_host = SPI3_HOST`, `freq_write/read = 10 MHz`.
- Skips the `_read_panel_id()` probe — bidirectional MOSI routing under IDF 6
  causes bus contention during the SIO read phase, so we trust the
  AXP2101@0x34 + AW9523@0x58 ID instead.
- A shorter custom `getInitCommands()` (SLPOUT → DISPON → INVON → COLMOD)
  plus per-call `cs_control` toggling matched to the new polling-transmit path.

### Local additions on top of issue #192

- **`Bus_SPI::writeBytes` watchdog yield** — `spi_device_polling_transmit`
  busy-waits the CPU, so a full-screen `pushImage` (≈ 37 × 4 KiB chunks)
  starves the IDLE task on the same core and trips Task WDT. We call
  `vTaskDelay(1)` every 16 chunks in `writeBytes` (matching the existing
  yield in `writeDataRepeat`).
- **M5Unified `RTC_PowerHub_Class::setAlarmIRQ`** — value-initialise
  `buf[3] = {0,0,0}` so GCC's stricter `-Werror=maybe-uninitialized` under
  IDF 6 is happy when none of the per-byte branches fire.

## Status

- Build: clean on IDF 6.0 for ESP32-S3 (CoreS3).
- LCD: panel autodetect + init OK; `fillScreen` / `pushSprite` render.
- Audio (Speaker / Mic): **shims allow compilation only**, runtime behaviour
  on IDF 6 not validated — the IDF 6 I²S API is materially different and
  audio paths will need a proper port before they work.
