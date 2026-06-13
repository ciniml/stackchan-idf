# stackchan-idf remote-flasher: host server

VPN 越しの ブラウザに WebSerial + esptool-js を担当させて、ローカルの
`idf.py` ラッパから 実機 (CoreS3) を flash するための host 中継サーバー。

```
idf.py wrapper / curl  --POST /flash (multipart)-->  this server
                                                       | WebSocket /ws
                                                       v
                                               remote browser tab
                                               (WebSerial + esptool-js)
                                                       |
                                                       v
                                                 ESP32 (CoreS3)
```

ブラウザ側 (esptool-js を動かす SPA) の仕様は `../PROTOCOL.md` 参照。

## 必要なもの

- Bun 1.0 以上 (`https://bun.sh/`)
- どこか VPN/Tunnel 内で到達可能な ホスト

## 起動

```sh
cd tools/remote-flasher/server
bun install
bun server.ts        # or `bun start`
```

環境変数:

| 変数 | デフォルト | 用途 |
|---|---|---|
| `PORT` | `8765` | リッスン ポート |
| `HOST` | `0.0.0.0` | bind アドレス |

`http://localhost:8765/` を開くと、ブラウザ接続状況と curl の例が見える
ステータス ページが返る。

## エンドポイント

### `GET /`
シンプルな ステータス HTML。

### `GET /ws`
ブラウザ クライアント用 WebSocket。**1 つだけ**受け付ける。2 つ目が来たら
既存接続に `{"type":"superseded"}` を送って切断し、新接続を採用する。

### `POST /flash`
`multipart/form-data`。フィールド:

- `meta` (text, JSON):
  ```json
  {
    "chip": "esp32s3",
    "baud": 460800,
    "erase": false,
    "sections": [
      {"name": "bootloader",      "offset": "0x0"},
      {"name": "partition-table", "offset": "0x8000"},
      {"name": "app",             "offset": "0x10000"}
    ]
  }
  ```
- それぞれの section に対応する **同じフィールド名** の file part。
  たとえば上の `meta` なら `bootloader`, `partition-table`, `app` の 3 つの
  file part が要る。

レスポンスは `text/event-stream` (chunked)。次の 2 種類のイベントを流す:

```
data: {"type":"progress","section":"app","written":12345,"total":40960}

data: {"type":"done","success":true}
```

`done.success=false` の場合は `error` フィールドにメッセージが入る。
タイムアウト 5 分。

ブラウザ未接続 → `503`、既に flash 中 → `409`、multipart parse 失敗 →
`400`。

### `POST /reset`
ブラウザに `{"type":"reset"}` を送り、esptool-js stub 経由で RTS/DTR
リセットさせる。レスポンスは `{"ok":true}`。

## curl の例

```sh
curl -N \
  -F 'meta={"chip":"esp32s3","baud":460800,"erase":false,"sections":[
       {"name":"bootloader","offset":"0x0"},
       {"name":"partition-table","offset":"0x8000"},
       {"name":"app","offset":"0x10000"}]};type=application/json' \
  -F 'bootloader=@build/bootloader/bootloader.bin' \
  -F 'partition-table=@build/partition_table/partition-table.bin' \
  -F 'app=@build/stackchan_idf.bin' \
  http://localhost:8765/flash
```

`-N` は curl の output buffering を切るためのオプションで、SSE 進捗を
リアルタイム表示するのに必要。

## ブラウザ側 URL

このサーバーは ブラウザ向けの SPA は配信しない。別エージェントが用意した
ビルド済 SPA (たとえば GitHub Pages にホストされたもの) を開き、本サーバー
の `ws://<host>:<port>/ws` を WebSocket URL として設定する。

## ログ

stdout に簡潔なログ:

- 接続 / 切断
- flash 開始
- セクションごと 10% 刻みの進捗
- 完了 / 失敗

## 制約

- 1 回に 1 つの flash しか走らない (排他)。
- 1 つの ブラウザだけ受付 (2 つ目は既存を蹴る)。
- WebSocket が落ちると 進行中の flash は即 失敗扱い。
