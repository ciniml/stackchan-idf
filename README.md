[English](README.en.md)

# stackchan-idf

M5Stack CoreS3 + Stack-chan ベース用のファームウェアです。ESP-IDF 5.4 / C++20 で書かれています。

## 機能

- **Avatar (顔描画)**: M5GFX で 30 fps 描画。呼吸 / saccade (眼球サッカード) / blink、6 表情 (Neutral / Happy / Sad / Angry / Doubt / Sleepy) と表情に応じたエフェクト。
- **サーボ制御**: SCS0009 (Yaw / Pitch) を UART1 (1 Mbps, GPIO 6/7) で制御。台形速度プロファイルの `PathGenerator` も含む。
- **スピーカー**: 起動音、ランダム babble (口パク連動)、AAC 録音再生に対応。
- **マイク**: 起動時に 2 秒録音 → ループバック再生で動作確認。
- **画面タッチ**: タップで 10 秒録音 → AAC エンコード → 再生。
- **吹き出し (Balloon)**: 画面下部に角丸の白パネル + 24 px の日本語ゴシック フォント。長文は marquee スクロール、表示完了で自動消去し、登録したコールバックを呼び出す。
- **Wi-Fi プロビジョニング**: 初回起動時に BLE (NimBLE) で **ESP Unified Provisioning** を実行。QR コードを画面に表示し、ESP BLE Provisioning アプリから SSID / パスワードを設定。資格情報は NVS に保存され、次回以降は自動接続。切断中は balloon に「Wi-Fi: 切断中」を表示。

## ハードウェア

- M5Stack CoreS3 (ESP32-S3、8 MB Quad-SPI PSRAM、16 MB Flash)
- Stack-chan ベース (PY32 IO Expander @ 0x6F、SCS0009 ×2)
  - 内部 I2C: AXP2101 (0x34) / AW9523 (0x58) / FT6336 (0x38) ほか M5Unified が管理
  - PY32 Pin 0: サーボ VM 電源 EN
  - サーボ バス (SCS0009): UART1, TX GPIO 6 / RX GPIO 7, 1 Mbps, 8 N 1
    - Yaw  ID = 1, zero_pos = 460
    - Pitch ID = 2, zero_pos = 620
  - 1 step ≈ 0.3125° (`deg = (raw - zero) * 5 / 16`)

## セットアップ

ESP-IDF 5.4 (本リポジトリは 5.4.2 で検証) を導入済みの環境で:

```sh
git clone <this repo>
cd stackchan-idf
git submodule update --init --recursive
tools/apply-m5-patches.sh                    # M5Unified の 1 行修正を適用
make set-target                              # 初回のみ
make build
make flash PORT=/dev/ttyACM0                 # 書き込み
make monitor PORT=/dev/ttyACM0               # シリアル モニタ
```

`tools/apply-m5-patches.sh` は upstream M5Unified の `RTC_PowerHub_Class::setAlarmIRQ` で
GCC 14 の `-Werror=maybe-uninitialized` に引っかかる `buf` 初期化を当てるだけです。

## 起動シーケンス

1. M5 / Avatar 初期化、起動音 (C5–E5–G5 アルペジオ)
2. **Wi-Fi**: NVS にクレデンシャル無し → BLE Provisioning (QR 表示)。あれば自動接続。Wi-Fi の取得待ちはせず即座に次へ。
3. マイク loopback (2 秒録音 → 再生)
4. サーボ電源 ON → ping (Yaw / Pitch)
5. Render Task (顔描画 30 fps, core 1) + Servo Task (20 ms 周期, core 0) を起動
6. demo_loop 開始 — ランダム babble + 10〜20 秒ごとのランダム ポーズ + 表情切替
7. 画面タップ → 10 秒 AAC 録音 → 再生

## リポジトリ構成

```
.
├── components/
│   ├── avatar/        顔描画 + アニメーション
│   ├── board/         CoreS3 HW 初期化と PY32 IO Expander
│   ├── scs_servo/     SCS0009 ドライバ + PathGenerator
│   ├── M5GFX/         submodule (upstream)
│   ├── M5Unified/     submodule (upstream + 1 patch)
│   └── tl_expected/   tl::expected backport (submodule)
├── main/              app_main, タスク、demo_loop、AAC、Wi-Fi prov、speech
├── patches/           upstream 修正パッチ
├── tools/             apply-m5-patches.sh, monitor_log.py
├── partitions.csv     OTA 配置
├── sdkconfig.defaults
└── Makefile           idf.py の薄いラッパ
```

## ライセンス

このリポジトリの自前ソース (`components/board`, `components/scs_servo`, `components/avatar`, `main`, `tools`)
は **Boost Software License 1.0** ([LICENSE](LICENSE)) の下で配布されます。

Submodule (`components/M5GFX` / `components/M5Unified` / `components/tl_expected/expected`)
と managed_components (`espressif/esp_audio_codec` ほか) はそれぞれの upstream ライセンスに従います。
