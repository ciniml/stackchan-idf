[日本語](README.md)

# stackchan-idf

Firmware for M5Stack CoreS3 + Stack-chan base, written against ESP-IDF 5.4 / C++20.

## Features

- **Avatar**: 30 fps face rendering with M5GFX — breath / saccade / blink animators and six expressions (Neutral / Happy / Sad / Angry / Doubt / Sleepy) with matching effects.
- **Servo control**: SCS0009 yaw + pitch over UART1 (1 Mbps, GPIO 6/7), including a trapezoidal-velocity `PathGenerator`.
- **Speaker**: Boot beep, randomised babble synced to the avatar's mouth, AAC record + playback.
- **Mic**: 2-second loopback (record-then-playback) sanity check at boot.
- **Touch**: Tap the LCD to record 10 seconds, encode to AAC, and play it back.
- **Speech balloon**: Rounded white panel along the bottom of the screen with a 24 px Japanese-capable Gothic font. Long text scrolls right-to-left as a marquee, auto-hides after one full pass, and invokes an optional completion callback.
- **BLE settings service**: a custom BLE GATT service (NimBLE) advertises from boot, always on. Configure the Wi-Fi network and the OpenAI API key from `tools/settings.html` (Web Bluetooth, desktop Chrome / Edge). The link is encrypted via Bluetooth 4.2+ Just Works pairing (no bonding). Settings are persisted in NVS; applying them reboots the device. While Wi-Fi is down the avatar pins a "Wi-Fi: 切断中" balloon.

## Hardware

- M5Stack CoreS3 (ESP32-S3, 8 MB Quad-SPI PSRAM, 16 MB flash)
- Stack-chan base (PY32 IO expander @ 0x6F, two SCS0009 servos)
  - Internal I²C: AXP2101 (0x34) / AW9523 (0x58) / FT6336 (0x38) — managed by M5Unified
  - PY32 pin 0: servo motor-voltage enable
  - Servo bus (SCS0009): UART1, TX GPIO 6 / RX GPIO 7, 1 Mbps, 8 N 1
    - Yaw  ID = 1, zero_pos = 460
    - Pitch ID = 2, zero_pos = 620
  - 1 step ≈ 0.3125° (`deg = (raw - zero) * 5 / 16`)

## Setup

With ESP-IDF 5.4 installed (tested against 5.4.2):

```sh
git clone <this repo>
cd stackchan-idf
git submodule update --init --recursive
tools/apply-m5-patches.sh                    # apply the one-line M5Unified fix
make set-target                              # first time only
make build
make flash PORT=/dev/ttyACM0
make monitor PORT=/dev/ttyACM0
```

`tools/apply-m5-patches.sh` just zero-initialises a `buf` array in
`M5Unified` `RTC_PowerHub_Class::setAlarmIRQ` so it stops tripping the
GCC 14 `-Werror=maybe-uninitialized` check.

## Boot sequence

1. M5 / Avatar initialised, startup arpeggio (C5–E5–G5).
2. **NVS / BLE settings service / Wi-Fi**: load settings from NVS, start the BLE settings service (always advertising). If an SSID is stored, start the Wi-Fi STA connection (non-blocking); otherwise boot continues with Wi-Fi disconnected until configured.
3. Mic loopback test (record 2 s, then play it back).
4. Servo power on, ping both servos.
5. Start the render task (30 fps face, core 1) and servo task (20 ms tick, core 0).
6. demo_loop begins — random babble, random head pose every 10–20 s, expression cycle.
7. Tap the LCD to record 10 s of audio as AAC and replay it.

## Repository layout

```
.
├── components/
│   ├── avatar/        face rendering + animators
│   ├── board/         CoreS3 HW bring-up + PY32 IO expander
│   ├── scs_servo/     SCS0009 driver + PathGenerator
│   ├── M5GFX/         submodule (upstream)
│   ├── M5Unified/     submodule (upstream + 1 patch)
│   └── tl_expected/   tl::expected backport (submodule)
├── main/              app_main, render/servo tasks, demo loop, AAC, Wi-Fi prov, speech
├── patches/           upstream-targeted patches
├── tools/             apply-m5-patches.sh, monitor_log.py
├── partitions.csv     OTA partition layout
├── sdkconfig.defaults
└── Makefile           thin wrapper around idf.py
```

## License

First-party sources (`components/board`, `components/scs_servo`,
`components/avatar`, `main`, `tools`) are released under the
**Boost Software License 1.0** ([LICENSE](LICENSE)).

The submodules (`components/M5GFX`, `components/M5Unified`,
`components/tl_expected/expected`) and managed_components
(`espressif/esp_audio_codec` etc.) keep their respective upstream licenses.
