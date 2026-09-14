# Stack-chan Recovery

[ADR-001](../docs/adr-001-extensible-flash-layout.md) の「固定領域に置く
小規模 Recovery アプリ」。BLE / Wi-Fi で Main イメージを受信して書き込む
ことだけを行い、表示・音声・サーボは持たない。

## 状態 (2026-09-11)

ADR-001 の最初の作業「Recovery の最小構成をビルドしてサイズを確定する」の
成果物。2026-09-15 に CoreS3 (ttyACM1) で実機スモーク済み:

- 起動 → BLE 広告 (NVS の機体名) / Wi-Fi STA 接続 / HTTP 待受: OK
- `GET /api/status`, `GET /api/ota/status`: OK
- `POST /api/ota/release {"tag":"v0.12.0"}` → GitHub Pages から 3.66 MB を
  約 24 秒で取得 → `main` (0x190000) へ書き込み → 再起動で Main v0.12.0 が
  起動し `ready` / HTTP 応答まで確認: OK
- BLE OTA 経路: **未確認** (Main 側に戻ってしまうので、bootctl で
  Recovery ↔ Main を切り替えられるようになってから検証する)
- 注意: 暫定表では OTA 型が `main` 1 つだけなので、Main の
  `esp_ota_get_next_update_partition()` は実行中の自分自身を返す
  (HMM 音声フォールバックが自スロットを選んだ)。ADR-001 の未決事項に追記済み。

| ビルド | イメージ サイズ | 1.5 MiB 枠に対する余裕 |
|---|---|---|
| cores3 (16 MB, board kind 0) | 0xed250 = 971,344 B (約 0.93 MiB) | 0x92db0 = 601,520 B (38 %) |
| atoms3 (8 MB, board kind 3)  | 同じ (flash サイズとボード種別だけが違う) | 同じ |

構成: NimBLE (peripheral のみ) + Wi-Fi STA + esp_http_server + esp_http_client
+ mbedTLS (CA バンドルは "common" 縮小版) + `-Os` + PSRAM なし。
フレームワーク内訳の上位は net80211 / cert bundle (esp_app_format) /
btdm_app / bt / mbedcrypto / pp / lwip。

## 何を流用しているか

`main/CMakeLists.txt` が `components/` の下記ソースを **そのままコンパイル**
する (サイズ計測が実コードと乖離しないようにするため)。

- `config_service/ota.cpp` — `esp_ota_begin/write/end` と状態 JSON
- `config_service/crypto.cpp` — BLE セッション暗号 (X25519 + HKDF + AES-GCM)
- `wifi_config_service/release_ota.cpp`, `https_fetch.cpp` — GitHub Pages からの取得

新規に書いたのは glue だけ: `main.cpp` (NVS 読み出し)、`wifi.cpp` (STA)、
`ble.cpp` (KeyExchange / OtaControl / OtaData の 3 characteristic)、
`http.cpp` (`/api/ota/*` と `/api/ota/release`)。UUID・ルート・NVS キーは
Main と同一なので、`tools/ble-cli` や設定ページの OTA フローがそのまま使える
想定。

## 既知の制限 (ADR-001 の次の作業で解消する)

- `ota.cpp` の `project_name` 検査は `set_expected_project_name("stackchan_idf")`
  で Main のイメージを受け入れるようにしてある (Main 側は未設定 = 自分自身と
  比較、従来どおり)。
- 書き込み先は `esp_ota_get_next_update_partition()` = 標準テーブル上の
  `ota_0`。ADR-001 の拡張テーブル + `bootctl` はまだ無く、
  `esp_ota_set_boot_partition` (otadata) で起動先を切り替えている。
- `partitions_recovery_*.csv` は **計測用の暫定表**。ADR-001 の最終
  レイアウトではない。この表を書き込むと現行の Main / storage / voice は
  消える (USB 書き込み必須)。
- Wi-Fi の SoftAP (プロビジョニング) は入れていない。NVS に SSID が無い
  機体は BLE 経由でのみ更新できる。

## ビルド

```sh
cd recovery
make set-target BOARD=cores3     # cores3 | stopwatch | atoms3r | atoms3
make build BOARD=cores3
make size-components BOARD=cores3
```
