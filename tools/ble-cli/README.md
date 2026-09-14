# stackchan-ble — BLE 設定クライアント (動作確認用)

stackchan-idf の BLE 設定インターフェース(config_service)を CLI から叩く Rust ツール。
[tools/settings.html](../settings.html) の Web Bluetooth フローと**同一のアプリ層セッション**を
実装するので、設定インターフェース側の変更を実機で回帰確認できる。

- ハンドシェイク: KeyExchange で X25519 ECDH → HKDF-SHA256
  (salt = `SHA-256(password)`, info = `stackchan-config-v1`) → AES-256 鍵
- 暗号: AES-256-GCM、AAD 無し、wire = `[12B nonce][ciphertext][16B tag]`
- Apply: 暗号化 write `0x01` → NVS 保存 → 約 200ms 後に再起動

## ビルド

```sh
cargo build --release      # target/release/stackchan-ble
```

Linux では BlueZ + DBus が必要(BT アダプタが UP であること)。

## 使い方

```sh
# 近くの BLE デバイスを列挙
stackchan-ble scan

# ハンドシェイク + Status(2バイト: flags, wifi_connected)を暗号 read
stackchan-ble status --device IDF --password <pw>

# operation-mode を BLE で設定 (既定で Apply=再起動)。0..5
#   0=MicLipSync 1=JttsRandom 2=Conversation 3=AsrLocal 4=EspNowRemote 5=EspNowSender
stackchan-ble set-mode 5 --device IDF --password <pw>
stackchan-ble set-mode 2 --device IDF --password <pw> --no-apply   # staging のみ

# 任意 characteristic を暗号 read / write (UUID 直指定・値は hex)
stackchan-ble read  e3f0a005-7b1c-4d2a-9e6f-2c5a8d4b1f00 --device IDF --password <pw>
stackchan-ble write e3f0a025-7b1c-4d2a-9e6f-2c5a8d4b1f00 05    --device IDF --password <pw>
```

- `--device` は advertising 名の部分一致(既定 `stackchan`)。名前非公開でも当該
  Service UUID を advertise していれば拾う。MAC 形式(`AA:BB:..`)を渡せば address 一致。
- `--password` は auth-password(未設定デバイスなら省略可)。誤ると GCM 認証で復号失敗する。

## 動作確認済み (2026-07-22, CoreS3 実機)

BLE 名 `ｽﾀｯｸﾁｬﾝIDF`(auth 有り)に対し:
- X25519+AES-256-GCM ハンドシェイク成立、Status を暗号 read/デコード
- `set-mode 5`(EspNowSender)→ Apply → 実機が op_mode=5 で起動を serial 確認
- `set-mode 3`(AsrLocal)→ 復帰まで **BLE のみで双方向にモード切替**を実証

ESP-NOW モード中も config_service(BLE)は稼働するので、httpd が無い ESP-NOW モードの
出入りにこの CLI が使える(公式リモコン検証の常用ツール)。

## 注意

- BlueZ は連続再接続で稀に `le-connection-abort-by-local` を返す。数秒空けて再実行する。
- espnow-channel / espnow-receiver-id は現状 BLE characteristic 未公開(HTTP `/api/settings`
  かオンデバイス UI で設定)。既定(ch1/id1)で足りる用途はこの CLI だけで完結する。

## OTA (ADR-001 レイアウト)

```sh
cargo run --release -- ota ../../build-cores3/stackchan_idf.bin
# → begin -> {"state":"failed",...,"error":"rebooting to recovery"}
#   Main は bootctl に「起動先=Recovery」を書いて再起動する。10 秒ほど待って再実行:
cargo run --release -- ota ../../build-cores3/stackchan_idf.bin
# → Recovery (同じ名前・同じ characteristic) が受信し、完了で Main を起動する。
```

チャンクは平文 480 B (暗号化で +28 B、ATT MTU 517 に収まる) を WriteWithoutResponse で
送り、`--check-every` (既定 32) チャンクごとに OtaControl の状態 JSON を読んで受信済み
バイト数が追いついているか確認する。実測 16 KiB/s (CoreS3、BlueZ)。
