# M5Stack 公式 Stack-chan ESP-NOW プロトコル調査 + 互換実装検討

作成: 2026-07-14 / 未追跡ローカル調査メモ
対象リポジトリ: `github.com/m5stack/StackChan`(公式ファーム/リモコン/アプリ/サーバ)

## 1. プロトコル(確定)

### ペイロード(双方向・8 バイト固定・リトルエンディアン)
`firmware/main/apps/app_espnow_ctrl/app_espnow_ctrl.cpp`(`handle_received_data` / `handle_send_pose`):

| offset | 型 | フィールド | 範囲 / 意味 |
|---|---|---|---|
| 0 | u8 | target-id | 0 = ブロードキャスト、1–254 = 特定受信機の Receiver ID |
| 1–2 | i16 LE | yaw-angle | -1280 ~ 1280 |
| 3–4 | i16 LE | pitch-angle | 0 ~ 900 |
| 5–6 | i16 LE | speed | 0 ~ 1000(推奨 600、送信側は 800 固定) |
| 7 | u8 | laser-enabled | 0 = off, 1 = on |

- 受信側: `target_id` が 0 か自分の `_receiver_id` に一致する時のみ採用 →
  `motion.moveWithSpeed(yaw, pitch, speed)` + `setLaserEnabled(laser)`。
- 送信側(Sender ロールの Stack-chan): 50ms 毎に自分の現在姿勢
  (`getCurrentYawAngle/PitchAngle`, speed=800, laser=0)を送出 → **デバイス間の姿勢ミラーリング**。
- 角度単位: 範囲(yaw ±1280 / pitch 0–900)から **0.1 度**が最有力(yaw ±128.0°, pitch 0–90.0°)。
  厳密互換には M5Stack 側 `stackchan::motion` の単位を要確認。

### 宛先・チャネル
- **WiFi チャネル(1–13)を送受で一致**させる必要(ESP-NOW は同一チャネルのみ到達)。
- 宛先は **payload の target-id** で論理的に分離(MAC ではなく broadcast + id フィルタ)。
- ロール(Receiver / Sender)・チャネル・ID はオンデバイス メニューで選択
  (`app_espnow_ctrl.cpp` の startup/advanced ページ)。Receiver は id 1–254、Sender は 0=broadcast も可。

## 2. トランスポート(互換の要)

**両側とも Espressif の `espnow` 管理コンポーネント**(`espnow.h` / `espnow_storage.h` /
`espnow_utils.h`)を使用。**生の `esp_now_send/recv` ではない**。

- 送信: `espnow_send(ESPNOW_DATA_TYPE_DATA, ESPNOW_ADDR_BROADCAST, pkt, len, &frame_head, portMAX_DELAY)`
  / `frame_head = ESPNOW_FRAME_CONFIG_DEFAULT()`。
- 受信: `espnow_set_config_for_data_type(ESPNOW_DATA_TYPE_DATA, true, cb)` → cb には
  **ライブラリ ヘッダを剥がした 8 バイト ペイロード**が渡る。
- WiFi は `WIFI_MODE_STA` + `esp_wifi_set_channel(ch)`(promiscuous で一旦ロック)。AP 接続はしない。
- config(送受共通):
  ```
  forward_enable = false;            // 多段転送ヘッダを付けない(Arduino 互換)
  forward_switch_channel = false;
  send_retry_num = 5;
  receive_enable.forward = false;
  receive_enable.data = true;        // ← 受信側のみ true(通常データ受信を有効化)
  ```

**互換上の含意**: on-wire は `[esp-now ライブラリ フレーム][8B ペイロード]`。**生 esp_now 実装では
非互換**の可能性が高い(ライブラリ ヘッダが無い)。→ 互換実装は **同じ espressif/esp-now
コンポーネントを同一設定で使う**のが確実。
- ソース中の "Arduino 互換" コメントは forward ヘッダを付けない意図。生 Arduino esp_now 送信との
  相互運用可否(ライブラリがヘッダ無しパケットを data として受けるか)は**要実機確認**の残論点。

## 3. stackchan-idf への互換実装 — 可否と方針

### 可否: **実装可能**(中工数)。ただし WiFi 排他が構造的制約。

- ESP-NOW は**固定チャネル + AP 非接続**が前提 → 既存の httpd 設定サーバ / 会話 WSS / OTA と
  **同時には成立しない**(WiFi STA が別チャネルの AP に繋がっていると届かない)。
  → 公式同様、**専用の動作モード**(ESP-NOW 時は設定サーバ等を起動しない)にするのが自然。
- **`OperationMode` に追記**(append-only 契約): 例 `EspNowReceiver = 4`? …現状 `AsrLocal=3` の次。
  Receiver(他機/リモコンに操作される)を主とし、Sender(自機姿勢を配信)も任意で。
- **サーボ写像**: 受信 yaw/pitch(0.1° 想定)→ 度 → stackchan-idf の SCS0009
  (`deg=(raw-zero)*5/16`, yaw zero=460 / pitch zero=620, 1step≈0.3125°)。`PathGenerator`
  (台形速度)で speed を反映。可動範囲は servo-limits でクランプ。laser は該当 GPIO 無ければ
  LED 等に読み替え or 無視。
- **プロビジョニング**: ESP-NOW モードでは httpd 非稼働のため、チャネル/Receiver-ID は
  **BLE(config_service)or 事前設定**で NVS に持たせる(公式はオンデバイス メニュー)。
  即時反映化の枠組み(本 repo の settings redesign)に espnow-channel / espnow-role /
  espnow-id を足す形。
- **依存追加**: `espressif/esp-now` 管理コンポーネント(+ espnow_storage/utils)。ESP-IDF 5.5 で
  利用可か、既存 WiFi/netif 初期化と競合しないかを要確認(esp-now は自前で WiFi を触る)。

### 実装ステップ(案)
1. `espressif/esp-now` を idf_component.yml に追加、最小の送受信 PoC(8B パケット)。
2. 新 `OperationMode`(EspNowReceiver / 任意で Sender)。ESP-NOW モードでは設定サーバ/会話を起動しない。
3. 受信 → yaw/pitch/speed を servo_task へ(既存 SharedState.servo.target_* + speed_override 経由)。
4. サーボ角の単位合わせ(M5Stack motion の実測 or 資料で 0.1° を確定)。
5. チャネル/ロール/ID を config_service(BLE)に追加。
6. 実機: M5Stack 公式リモコン(または公式 Stack-chan の Sender)から操作して相互運用を確認。

### 主要リスク / 未確定
- **[中] esp-now ライブラリの ESP-IDF 5.5 互換 & WiFi 初期化競合**(自前 netif と二重初期化しないか)。
- **[中] 角度単位**(0.1° 仮説の確証)。ズレると動くが範囲/向きが合わない。
- **[低] laser(GPIO2)** は CoreS3 に該当機能なし → 読み替え/無視。
- **[低] 生 Arduino 相互運用**(必要なら別途、ライブラリ ヘッダの有無を実測)。

## 本実装 (Phase C, 2026-07-22) — 完了・実機検証済み

`OperationMode::EspNowRemote = 4`(append-only)として本実装。公式リモコン互換の
受信で頭部を遠隔操作する。

- **新コンポーネント `components/espnow_remote/`**: WiFi 固定チャネル(AP 非接続)+
  espressif/esp-now を初期化し、8B ポーズを受信。`target-id` が 0 か自機
  `receiver_id` に一致する時だけ `PoseHandler` コールバックを呼ぶ(main の
  SharedState には依存しない疎結合 API)。
- **設定**: `espnow-channel`(1–13)/ `espnow-receiver-id`(1–254)を settings
  registry に追加(Staged, NVS `enow_ch`/`enow_id`)。`operation-mode` の
  上限を EspNowRemote に拡張(HTTP `/api/settings`・`/api/operation-mode`・
  GATT の各バリデーション)。
- **app_main 分岐**: `espnow_mode` で wifi_start / httpd / 会話 / wifi_audio /
  mic-lip-sync / ボイス ロード / boot イベントを起動しない。servo 電源+servo
  タスクは通常どおり起動し、受信ハンドラが `SharedState.servo.target_*` +
  `speed_override` を書く。`demo_loop` は `external_servo_control` フラグで
  アイドル頭振り(ランダム ポーズ / なでなで wobble)を抑制。
- **サーボ写像**: 受信 yaw/pitch(0.1° 生値)→ /10 で度 → `servo_limits` で
  クランプ → servo タスクが台形速度で追従。speed はゴール速度として素通し
  (0–1023 クランプ)。laser は CoreS3 に GPIO 無く現状無視(ログのみ)。
- **Kconfig**: `CONFIG_STACKCHAN_ESPNOW_REMOTE_ENABLED`(default y)。無効化で
  esp-now 依存と本モードを slim ビルドから外せる。選択肢への包含は
  `operation_mode_count`/`next_operation_mode` をリスト化して穴なく処理。

### 実機検証 (CoreS3=ttyACM0 / AtomS3 Lite ブリッジ=ttyACM1)

対抗機が無いため **AtomS3 Lite に PC↔ESP-NOW ブリッジ ファーム**
(`~/repos/espnow-stackchan-bridge`)を載せて公式互換パケットを送出:
- CoreS3 は `operation_mode=4 (espnow=1)` で起動、httpd スキップ、
  `espnow-rx: listening ... channel 1 (id 1)` を確認(クラッシュ無し)。
- ブリッジのスイープ送信 → CoreS3 が受信し `espnow pose: yaw=157 ... -> 15.7 deg`
  等、0.1°→度の写像が正確。pitch は `-114 -> -10.0`(limits クランプ)を実証。
  RSSI −32〜−35dBm。頭部が追従スイープ。
- 他モード(ASR 等)は従来どおり起動する回帰確認済み。
- **プロビジョニング経路**: ESP-NOW モード中は httpd が無いので、別モードで
  `/api/settings`(channel/id/operation-mode)+ `/api/apply` 再起動、または
  BLE/オンデバイス UI で設定してからモード切替(公式のオンデバイス メニュー相当)。

### 残課題(公式リモコン相互運用)
- 角度単位 0.1° は範囲から確度高いが、**公式実機との厳密な向き/ゼロ点の一致は
  未検証**(手元に対抗機/公式機が無いため)。ブリッジ経由の写像は正確に動作。
- 生 Arduino esp_now(ライブラリ ヘッダ無し)との相互運用は未確認(§2 の残論点)。
- `components/espnow_poc/`(PoC)は本実装で役目終了。gate off のまま残置(将来削除可)。

## 参照ファイル(m5stack/StackChan)
- `firmware/main/apps/app_espnow_ctrl/app_espnow_ctrl.cpp`(プロトコル本体・8Bパケット・role/ch/id UI)
- `firmware/main/hal/hal_espnow.cpp`(WiFi/ESP-NOW 初期化・送信・laser GPIO2)
- `remote/code/main/esp_now/esp_now_init.c`(リモコン送信の on-wire・同一設定)
- `remote/code/main/StackChan-RemoteControl-ESPNow.cpp` / `joystick/joystick_handle.c`(JoyC→パケット)
</content>
