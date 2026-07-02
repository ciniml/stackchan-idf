[English](README.en.md)

# stackchan-idf

M5Stack の CoreS3 / AtomS3R / AtomS3 / StopWatch (C152) で動く Stack-chan ファームウェア。
ESP-IDF 5.5 / C++20。AI 音声対話 (OpenAI / Gemini / XiaoZhi)、BLE / Wi-Fi / SoftAP の 3 経路
設定、デバイス側 OTA をサポートします。

## Web Flasher / 設定ページ

GitHub Releases に置かれたファームウェアをブラウザから書き込めます (Chrome / Edge):

- **書き込み**: <https://ciniml.github.io/stackchan-idf/>
- **BLE 設定**: <https://ciniml.github.io/stackchan-idf/settings.html> (Web Bluetooth、デスクトップ Chrome / Edge のみ)
- **Wi-Fi 設定**: デバイスを Wi-Fi に繋いだ後 `http://stackchan-XXXXXX.local/` (mDNS)
- **iOS / SoftAP 設定**: 本体ボタン (boards により方法が異なる、後述) で AP モードに入り、
  LCD に表示される Wi-Fi QR を iPhone Camera で読む → captive portal で設定ページが自動表示

タグ `vX.Y.Z` を push すると CI が 4 ボード分ビルドして Release を作り、
Pages サイトに反映されます。

## 対応ボード

| ボード | 略号 | 表示 | 主な特徴 |
|---|---|---|---|
| CoreS3 + Stack-chan ベース | `cores3` | 320×240 IPS + touch | 標準。サーボ 2 軸、頭タッチ センサー、INA226 電池計 |
| CoreS3 + Takao Base | `cores3` | 同上 | Port A 半二重サーボ、サーボ電源/電池計なし |
| AtomS3R + Atomic ECHO BASE (アトムニャン) | `atoms3r` | 128×128 LCD | サーボなし、ES8311 音声、BtnA で UI / AP |
| AtomS3 (PSRAM なし) + ECHO BASE | `atoms3` | 128×128 LCD | 軽量プロファイル、会話/RTP 非対応 |
| M5 StopWatch (C152) | `stopwatch` | 466×466 円形 AMOLED + touch | サーボなし、touch で視線追従、ES8311 音声 |

ビルドは `make build BOARD=<略号>` (デフォルト `cores3`)。
ボードは起動時に検出され、`set_board_kind()` で UI / 機能のグレーアウトに反映されます。

## 機能

- **AI 音声対話**: OpenAI Realtime / Google Gemini Live / XiaoZhi サーバーに WebSocket
  接続。マイク入力 → 応答音声 → 口パク連動。半二重 CoreS3 では発話中マイクを止め、
  応答中は LCD タップ / 頭タッチ (Si12T) で barge-in 可能。サーバー側 VAD で
  ターン検出。OpenAI / Gemini ではツール (`set_expression` / `set_head_pose` /
  `speak_katakoto`) で表情・首振り・カタコト声を制御、XiaoZhi は感情を表情にマップ。
  応答テキストは吹き出しに進行表示。
- **アバター描画**: M5GFX で 30 fps。呼吸 / saccade / blink、6 表情 (Neutral / Happy
  / Sad / Angry / Doubt / Sleepy)。**Avatar DSL** (`.avdsl` ソース → `.avbc` バイトコード)
  で顔のレイアウトとアニメーションを差し替え可能 (BLE / Wi-Fi 経由でライブ更新)。
- **マイク連動口パク (lip sync)**: FFT + 帯域 log + spectral flux で開口度推定、
  雑音床 EWMA AGC で環境ノイズに追従。
- **サーボ**: SCS0009 yaw + pitch を UART1 (1 Mbps) で制御。台形速度
  プロファイル `PathGenerator`、駆動時のみトルク有効化。ボード別レンジ
  キャリブレーション (ServoLimits) を NVS 保存。
- **スピーカー / オーディオ**: 起動音 (C5–E5–G5、設定で OFF 可)、jtts ランダム
  babble、AAC 録音再生、BLE オーディオ ストリーム、Wi-Fi RTP (L16 / μ-law / AAC) 受信。
  音量は **0..200%** をライブ制御 (BLE / Wi-Fi / 本体 UI)。
- **NeoPixel**: nekomimi 用 LED ストリップ アニメーション (虹 / 単色 / リップシンク
  レベルメーター モード)。
- **LT タイマー**: 発表時間アシスト。残り N 秒で予告 → 時刻ぴったり → 超過繰返し
  アナウンス (jtts)。
- **オンデバイス UI**:
  - CoreS3 / StopWatch (touch panel): 画面右上タップで 5 タブ UI (情報 / 設定 /
    操作 / 範囲 / 会話 / LT)
  - AtomS3R / AtomS3 (button only): BtnA 短押でステータス オーバーレイ、長押で
    operation_mode サイクル
- **吹き出し**: 24 px ゴシック フォントの白パネル、長文は marquee スクロール。
- **BLE 設定サービス** (NimBLE GATT): `tools/settings.html` (Web Bluetooth) から
  Wi-Fi 接続先・API キー・各種設定・OTA 更新。Bluetooth 4.2 以降の Just Works
  ペアリング + アプリケーション層 X25519 + AES-256-GCM 暗号化、optional password 認証。
  設定は NVS に保存、Apply で再起動して反映。
- **Wi-Fi 設定サービス**: 接続後 HTTP サーバー (port 80) + `settings_wifi.html`、
  mDNS (`stackchan-XXXXXX.local`)。SSID / プロバイダ / API キー / システム
  プロンプト / 追加 HTTP ヘッダ / jtts / Avatar DSL / OTA 全て対応。
- **SoftAP プロビジョニング (iOS フレンドリー)**: STA が未設定 / 接続失敗時に
  本体トリガで AP モード起動 (`Stackchan-XXXXXX` + WPA2)。LCD に Wi-Fi QR 表示、
  iPhone Camera スキャン → 接続 → **captive portal が自動で `settings_wifi.html` を開く**
  (DNS hijack + HTTP 404 catch-all)。AP モード中は require_auth が bypass され、
  設定が即触れる。
- **OTA 更新**: デュアル OTA パーティション、起動検証 + ロールバック対応。
  - **BLE 経由**: settings.html → 暗号化された chunk
  - **Wi-Fi ローカル ファイル**: settings_wifi.html で `.bin` をアップロード
  - **Wi-Fi デバイス側 fetch** (v0.7.4+): `POST /api/ota/release {tag}` で device
    自身が GitHub Pages から自分のボード用バイナリをダウンロード → 適用 (STA 必要)

## ハードウェア (CoreS3 + Stack-chan ベースの場合)

- M5Stack CoreS3 (ESP32-S3、8 MB Quad-SPI PSRAM、16 MB Flash)
- Stack-chan ベース (PY32 IO Expander @ 0x6F、SCS0009 ×2)
  - 内部 I²C: AXP2101 (0x34) / Touch (0x38) ほか M5Unified が管理
  - PY32 Pin 0: サーボ VM 電源 EN (ON 後 200 ms 待ってバス使用)
  - サーボ バス (SCS0009): UART1, TX GPIO 6 / RX GPIO 7, 1 Mbps, 8 N 1
    - Yaw  ID = 1, zero_pos = 460
    - Pitch ID = 2, zero_pos = 620
  - 1 step ≈ 0.3125° (`deg = (raw - zero) * 5 / 16`)
  - 頭タッチ センサー Si12T @ 0x68 (なでなで / barge-in、3 ゾーン)
- 他ボードのピン配置は各 `sdkconfig.defaults.<board>` と
  `components/board/board.cpp` を参照

## セットアップ

ESP-IDF 5.5 (本リポジトリは 5.5.4 で検証。5.4.2 でもビルド可) を導入済みの環境で:

```sh
git clone <this repo>
cd stackchan-idf
git submodule update --init --recursive
tools/apply-m5-patches.sh                    # M5Unified の 1 行修正を適用
make set-target BOARD=cores3                 # 初回のみ (BOARD 別に build dir が分かれる)
make build     BOARD=cores3
make flash     BOARD=cores3 PORT=/dev/ttyACM0
make monitor   BOARD=cores3 PORT=/dev/ttyACM0
```

`BOARD=` を `atoms3r` / `atoms3` / `stopwatch` に置き換えると該当ボード用の
ビルドが `build-<board>/` 配下に作られます (各 build dir は独立)。

`tools/apply-m5-patches.sh` は upstream M5Unified の
`RTC_PowerHub_Class::setAlarmIRQ` で GCC 14 の `-Werror=maybe-uninitialized`
に引っかかる `buf` 初期化を当てるだけです。

OpenAI / Gemini の API キーはビルドに埋め込まず、BLE / Wi-Fi 設定 IF から
実行時に投入します (NVS に保存)。`sdkconfig.defaults.local` (gitignore 済み) で
コンパイル時デフォルトを与えることもできます。

## 起動シーケンス (CoreS3 標準パス)

1. M5 / Avatar 初期化、起動音 (C5–E5–G5 アルペジオ、設定で OFF 可)
2. NVS から設定読み込み → BLE 設定サービス起動 (常時 advertising)
3. SSID があれば Wi-Fi STA 接続を非ブロッキング開始
   - STA 接続後 → mDNS + HTTP 設定サーバー + SNTP 開始
4. マイク loopback (2 秒録音 → 再生) で動作確認
5. サーボ電源 ON → ping (Yaw / Pitch) → 1.5 s 待機
6. Render Task (顔描画 30 fps, core 1) + Servo Task (20 ms 周期, core 0) を起動。
   会話が有効なら Conversation Task が Wi-Fi 接続を待って AI 対話を開始
7. demo_loop 開始 — 会話アイドル時はランダム babble + ランダム ポーズ +
   なでなで反応、会話中は AI 対話タスクがアバターと音声を制御
8. 画面右上タップでオンデバイス UI、応答中の画面タップで barge-in、
   操作タブの「AP モード」で SoftAP プロビジョニング

## リポジトリ構成

```
.
├── components/
│   ├── avatar/             顔描画 + アニメーション (Breath / Saccade / Blink、6 表情)
│   ├── avatar_vm/          Avatar DSL バイトコード VM + ストレージ
│   ├── board/              CoreS3 / AtomS3R / StopWatch HW 初期化 (ボード自動検出)
│   ├── scs_servo/          SCS0009 ドライバ + PathGenerator (台形速度)
│   ├── jtts/               日本語カタコト TTS (babble / speak_katakoto / LT 通知)
│   ├── conversation/       AI 音声対話クライアント (OpenAI / Gemini / XiaoZhi)
│   ├── config_service/     BLE GATT 設定サービス + NVS + OTA + X25519/AES-GCM
│   ├── wifi_config_service/ Wi-Fi HTTP 設定 + 内蔵 Web ページ + release OTA
│   ├── telegram/           Telegram Bot API (TLS) 通知クライアント (oss)
│   ├── M5GFX/              submodule (upstream)
│   ├── M5Unified/          submodule (upstream + 1 patch)
│   └── tl_expected/        tl::expected backport (submodule)
├── main/                   app_main, render/servo task, demo_loop, ap_screen,
│                           captive_portal, device_ui, atom_status, wifi_sta
├── patches/                upstream-targeted patches
├── tools/                  apply-m5-patches.sh, monitor_log.py, settings.html,
│                           avatar_dsl/ (コンパイラ + WASM 連携)
├── assets/                 .avdsl ソース (default_face, omega_mouth, aokko_face)
├── partitions.csv          OTA 配置 (ota_0 / ota_1 / nvs / storage)
├── sdkconfig.defaults*     共通 + ボード別 (.cores3 / .atoms3r / .atoms3 / .stopwatch)
└── Makefile                idf.py の薄いラッパ (BOARD= 切替)
```

## ライセンス

このリポジトリの自前ソース (`components/board`, `components/scs_servo`, `components/avatar`,
`components/avatar_vm`, `components/jtts`, `components/conversation`,
`components/config_service`, `components/wifi_config_service`,
`components/telegram`, `main`, `tools`) は
**Boost Software License 1.0** ([LICENSE](LICENSE)) の下で配布されます。

Submodule (`components/M5GFX` / `components/M5Unified` / `components/tl_expected/expected`)
と managed_components (`espressif/esp_audio_codec` / `espressif/esp_websocket_client` /
`espressif/mdns` / `espressif/esp_jpeg` / `espressif/esp32-camera` 等)
はそれぞれの upstream ライセンスに従います。
