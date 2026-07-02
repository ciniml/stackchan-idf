[日本語](README.md)

# stackchan-idf

Firmware for Stack-chan running on M5Stack CoreS3 / AtomS3R / AtomS3 / StopWatch (C152),
written against ESP-IDF 5.5 / C++20. Supports AI voice conversation (OpenAI / Gemini /
XiaoZhi), three configuration paths (BLE / Wi-Fi STA / SoftAP), and device-side OTA.

## Web Flasher / Settings page

Released firmware can be flashed straight from the browser (Chrome / Edge):

- **Flash**: <https://ciniml.github.io/stackchan-idf/>
- **BLE Settings**: <https://ciniml.github.io/stackchan-idf/settings.html> (Web Bluetooth, desktop Chrome / Edge only)
- **Wi-Fi Settings**: once the device is on Wi-Fi, `http://stackchan-XXXXXX.local/` (mDNS)
- **iOS / SoftAP Settings**: trigger AP mode on-device (per-board, see below), scan the
  Wi-Fi QR shown on the LCD with iPhone Camera → join → captive portal pops the
  settings page automatically

Push a tag `vX.Y.Z` and CI builds for all four boards, attaches the artifacts to
a Release, and the Pages site picks them up automatically.

## Supported boards

| Board | Slug | Display | Notable |
|---|---|---|---|
| CoreS3 + Stack-chan base | `cores3` | 320×240 IPS + touch | Default. 2 servos, head touch sensor, INA226 battery gauge |
| CoreS3 + Takao Base | `cores3` | same | Half-duplex servo on Port A; no servo VM control / no battery gauge |
| AtomS3R + Atomic ECHO BASE ("AtomNyan") | `atoms3r` | 128×128 LCD | No servos, ES8311 audio, BtnA-driven UI / AP toggle |
| AtomS3 (no PSRAM) + ECHO BASE | `atoms3` | 128×128 LCD | Slim profile, no conversation / RTP |
| M5 StopWatch (C152) | `stopwatch` | 466×466 round AMOLED + touch | No servos, gaze-follow on touch, ES8311 audio |

Build with `make build BOARD=<slug>` (default `cores3`). The board is detected at
boot and broadcast via `set_board_kind()` so UI tabs and feature toggles grey out
appropriately.

## Features

- **AI voice conversation**: WebSocket to one of OpenAI Realtime / Google Gemini
  Live / a XiaoZhi server, streaming mic input → reply audio with mouth-sync.
  The half-duplex CoreS3 mutes the mic while speaking; interrupt a reply
  (barge-in) with an LCD tap or a head touch (Si12T). Turns are detected by
  server-side VAD. OpenAI / Gemini drive expression, head pose and robotic
  speech via tools (`set_expression` / `set_head_pose` / `speak_katakoto`);
  XiaoZhi maps its reply emotion to an expression. Reply text streams into
  the balloon.
- **Avatar rendering**: 30 fps with M5GFX. Breath / saccade / blink animators,
  six expressions (Neutral / Happy / Sad / Angry / Doubt / Sleepy). The face
  layout and animation is driven by an **Avatar DSL** (`.avdsl` source →
  `.avbc` bytecode) that can be replaced live over BLE / Wi-Fi.
- **Mic-driven lip-sync**: FFT + per-band log + spectral flux estimate of mouth
  opening, with an EWMA noise-floor AGC for ambient drift.
- **Servos**: SCS0009 yaw + pitch over UART1 (1 Mbps). Trapezoidal-velocity
  `PathGenerator`; torque only engaged while moving. Per-board range
  calibration (ServoLimits) persisted in NVS.
- **Speaker / audio**: Boot arpeggio (C5–E5–G5, can be disabled), jtts random
  babble, AAC record + playback, BLE audio streaming, Wi-Fi RTP receive
  (L16 / μ-law / AAC). Volume is **0..200%**, live-controlled from BLE /
  Wi-Fi / on-device UI.
- **NeoPixel**: Nekomimi LED-strip animations (rainbow / solid / lip-sync
  level-meter mode).
- **LT timer**: Talk-time assistant. "Soon" notice N seconds before the end,
  on-the-dot announcement, repeated over-time call-outs (jtts).
- **On-device UI**:
  - CoreS3 / StopWatch (touch panel): tap top-right corner for a 5-tab UI
    (info / settings / control / range / conversation / LT)
  - AtomS3R / AtomS3 (button only): BtnA short-press toggles the status
    overlay, long-press cycles `operation_mode`
- **Speech balloon**: 24 px Japanese-capable Gothic font on a rounded white
  panel; long text scrolls right-to-left as a marquee.
- **BLE settings service** (NimBLE GATT): configure Wi-Fi, API keys and OTA
  from `tools/settings.html` (Web Bluetooth). Bluetooth 4.2+ Just Works
  pairing plus an application-layer X25519 + AES-256-GCM session, with
  optional password auth. Settings land in NVS; Apply reboots.
- **Wi-Fi settings service**: once Wi-Fi connects, the device serves an HTTP
  server (port 80) + a built-in `settings_wifi.html`, advertised over mDNS
  (`stackchan-XXXXXX.local`). Covers SSID, provider, API keys, system prompt,
  extra HTTP headers, jtts, Avatar DSL, OTA — everything the BLE page covers.
- **SoftAP provisioning (iOS-friendly)**: when STA is unset / failing, trigger
  AP mode on-device (`Stackchan-XXXXXX` + WPA2). LCD shows a Wi-Fi QR; the
  iPhone Camera scans → joins → **the captive portal automatically opens
  `settings_wifi.html`** (DNS hijack + HTTP 404 catch-all). `require_auth`
  is bypassed while AP is up, so the settings UI is immediately reachable.
- **OTA updates**: dual OTA partitions, boot verification with rollback.
  - **BLE**: settings.html → encrypted chunks
  - **Wi-Fi local file**: upload a `.bin` from settings_wifi.html
  - **Wi-Fi device-side fetch** (v0.7.4+): `POST /api/ota/release {tag}` —
    the device pulls its own per-board binary from GitHub Pages over its
    STA link and applies it (STA must be up).

## Hardware (CoreS3 + Stack-chan base path)

- M5Stack CoreS3 (ESP32-S3, 8 MB Quad-SPI PSRAM, 16 MB flash)
- Stack-chan base (PY32 IO expander @ 0x6F, two SCS0009 servos)
  - Internal I²C: AXP2101 (0x34) / touch (0x38) — managed by M5Unified
  - PY32 pin 0: servo motor-voltage enable (wait 200 ms after ON before using the bus)
  - Servo bus (SCS0009): UART1, TX GPIO 6 / RX GPIO 7, 1 Mbps, 8 N 1
    - Yaw  ID = 1, zero_pos = 460
    - Pitch ID = 2, zero_pos = 620
  - 1 step ≈ 0.3125° (`deg = (raw - zero) * 5 / 16`)
  - Head touch sensor Si12T @ 0x68 (3 zones, for nadenade / barge-in)
- Other boards' pin layout: see each `sdkconfig.defaults.<board>` and
  `components/board/board.cpp`.

## Setup

With ESP-IDF 5.5 installed (tested against 5.5.4; 5.4.2 still builds):

```sh
git clone <this repo>
cd stackchan-idf
git submodule update --init --recursive
tools/apply-m5-patches.sh                    # apply the one-line M5Unified fix
make set-target BOARD=cores3                 # first time only; per-board build dirs
make build      BOARD=cores3
make flash      BOARD=cores3 PORT=/dev/ttyACM0
make monitor    BOARD=cores3 PORT=/dev/ttyACM0
```

Replace `BOARD=` with `atoms3r` / `atoms3` / `stopwatch` to build for those
boards; each lands under its own `build-<board>/` directory.

`tools/apply-m5-patches.sh` just zero-initialises a `buf` array in
`M5Unified` `RTC_PowerHub_Class::setAlarmIRQ` so it stops tripping the
GCC 14 `-Werror=maybe-uninitialized` check.

API keys for OpenAI / Gemini are not baked into the build — supply them at
runtime from the BLE / Wi-Fi settings interface (stored in NVS). A
compile-time default can be given via `sdkconfig.defaults.local` (gitignored).

## Boot sequence (CoreS3 default path)

1. M5 / Avatar init, startup arpeggio (C5–E5–G5, can be disabled).
2. Load settings from NVS, start the BLE settings service (always advertising).
3. If an SSID is stored, start the Wi-Fi STA connection (non-blocking).
   On STA up → start mDNS + HTTP config server + SNTP.
4. Mic loopback test (record 2 s, play it back) as a sanity check.
5. Servo power on → ping (Yaw / Pitch) → 1.5 s settle.
6. Start the render task (30 fps face, core 1) and servo task (20 ms tick,
   core 0). If conversation is enabled, the conversation task waits for
   Wi-Fi and starts the AI dialogue.
7. demo_loop begins — idle: random babble, random head pose, nadenade
   reactions; during a conversation the AI task drives avatar + audio.
8. Tap top-right for the on-device UI; tap the screen during a reply to
   barge in; the Control tab's "AP モード" row enters SoftAP provisioning.

## Repository layout

```
.
├── components/
│   ├── avatar/              face rendering + animators (breath / saccade / blink, 6 expressions)
│   ├── avatar_vm/           Avatar DSL bytecode VM + NVS storage
│   ├── board/               CoreS3 / AtomS3R / StopWatch HW bring-up (board auto-detect)
│   ├── scs_servo/           SCS0009 driver + PathGenerator (trapezoidal velocity)
│   ├── jtts/                Japanese katakoto TTS (babble / speak_katakoto / LT calls)
│   ├── conversation/        AI voice-conversation clients (OpenAI / Gemini / XiaoZhi)
│   ├── config_service/      BLE GATT settings service + NVS + OTA + X25519/AES-GCM
│   ├── wifi_config_service/ Wi-Fi HTTP config + built-in web page + release OTA
│   ├── telegram/            Telegram Bot API (TLS) notification client (oss)
│   ├── M5GFX/               submodule (upstream)
│   ├── M5Unified/           submodule (upstream + 1 patch)
│   └── tl_expected/         tl::expected backport (submodule)
├── main/                    app_main, render/servo task, demo_loop, ap_screen,
│                            captive_portal, device_ui, atom_status, wifi_sta
├── patches/                 upstream-targeted patches
├── tools/                   apply-m5-patches.sh, monitor_log.py, settings.html,
│                            avatar_dsl/ (compiler + WASM glue)
├── assets/                  .avdsl sources (default_face, omega_mouth, aokko_face)
├── partitions.csv           OTA layout (ota_0 / ota_1 / nvs / storage)
├── sdkconfig.defaults*      shared + per-board (.cores3 / .atoms3r / .atoms3 / .stopwatch)
└── Makefile                 thin wrapper around idf.py (BOARD= switch)
```

## License

First-party sources (`components/board`, `components/scs_servo`,
`components/avatar`, `components/avatar_vm`, `components/jtts`,
`components/conversation`, `components/config_service`,
`components/wifi_config_service`, `components/telegram`, `main`, `tools`)
are released under the **Boost Software License 1.0** ([LICENSE](LICENSE)).

The submodules (`components/M5GFX`, `components/M5Unified`,
`components/tl_expected/expected`) and managed_components
(`espressif/esp_audio_codec`, `espressif/esp_websocket_client`,
`espressif/mdns`, `espressif/esp_jpeg`, `espressif/esp32-camera`, etc.)
keep their respective upstream licenses.
