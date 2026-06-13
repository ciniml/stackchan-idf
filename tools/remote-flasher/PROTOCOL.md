# remote-flasher WebSocket protocol

ホスト サーバー (`server/server.ts`) と ブラウザ クライアント
(WebSerial + esptool-js を担当する別エージェントの SPA) が話す プロトコル。

WS URL: `ws://<host>:<port>/ws` (デフォルト `8765`)。

接続は **常に 1 つだけ**。2 つ目が来たら、サーバーは既存接続に
`{"type":"superseded"}` を送ってから close、その後に新接続を採用する。

両端ともテキスト フレーム = JSON、バイナリ フレーム = 生バイト列、
で使い分ける。

## フレーム フォーマット概要

| 方向 | フレーム | 用途 |
|---|---|---|
| host → browser | text JSON `flash_request` | これから N セクション書き込む宣言 |
| host → browser | binary | セクション本体 (宣言順に 1 セクション = 1 フレーム) |
| host → browser | text JSON `reset` | リセットのみ実施 |
| host → browser | text JSON `ping` | 15 s heartbeat |
| browser → host | text JSON `hello` | 接続直後の自己紹介 |
| browser → host | text JSON `progress` | esptool-js の写経 |
| browser → host | text JSON `done` | flash 完了 / 失敗 |
| browser → host | text JSON `log` | esptool-js のログ転写 |
| browser → host | text JSON `serial` | ESP シリアル モニタ 1 行 (`/monitor` SSE へ fan-out) |
| browser → host | text JSON `pong` | heartbeat 応答 |

## host → browser

### `flash_request`
```json
{
  "type": "flash_request",
  "id": "<uuid>",
  "chip": "esp32s3",
  "baud": 460800,
  "erase": false,
  "sections": [
    {"name": "bootloader",      "offset": "0x0",     "size": 21456},
    {"name": "partition-table", "offset": "0x8000",  "size": 3072},
    {"name": "app",             "offset": "0x10000", "size": 1438720}
  ]
}
```

- `id`: この job を識別する UUID。`progress` / `done` で必ず送り返すこと。
- `offset`: 16 進文字列 (`0x` 前置)。esptool-js には数値化して渡す。
- `size`: バイト数。`sections` 配列の順序で、この直後に同数のバイナリ
  フレームが届く。

### バイナリ フレーム (セクション本体)
`flash_request` 直後に、宣言順に N 個のバイナリ フレームが届く。各
フレームは 1 セクション全体。チャンク分割は無し (将来追加するなら明示
プロトコル変更)。

### `reset`
```json
{ "type": "reset" }
```
RTS/DTR でリセットだけ行う指示。esptool-js stub 起動は任意。

### `ping`
```json
{ "type": "ping" }
```
15 秒ごとに送る。ブラウザは `pong` で返す。

### `superseded`
```json
{ "type": "superseded" }
```
新しい接続が来たので閉じる旨の通知。受信したら ブラウザ側もクリーンに
切断してよい (新タブで開いた等)。

## browser → host

### `hello`
```json
{
  "type": "hello",
  "userAgent": "Mozilla/5.0 ...",
  "portInfo": { "usbVendorId": 12346, "usbProductId": 32769 }
}
```
接続直後に 1 度だけ送る。`portInfo` は `SerialPort.getInfo()` の結果。
無くてもサーバーは受け付けるが、ステータス ページに UA は出る。

### `progress`
```json
{
  "type": "progress",
  "id": "<uuid>",
  "section": "app",
  "written": 12345,
  "total": 1438720
}
```
esptool-js の write callback から発火。`section` は `flash_request.sections`
の `name` と一致させる。`total` は省略可だが、初回は付けるべき。

### `done`
```json
{
  "type": "done",
  "id": "<uuid>",
  "success": true
}
```
失敗時:
```json
{
  "type": "done",
  "id": "<uuid>",
  "success": false,
  "error": "MD5 mismatch on section 'app'"
}
```
`id` がマッチしない (古い job の遅延 done など) ものはサーバー側で無視
される。

### `log`
```json
{
  "type": "log",
  "level": "info",
  "message": "Hard resetting via RTS pin..."
}
```
`level` は `info` / `warn` / `error`。サーバーは stdout にプレフィクス
付きで転写する。デバッグ用。

### `serial`
```json
{
  "type": "serial",
  "data": "I (12345) tag: hello from ESP"
}
```
Monitor 中、ブラウザが WebSerial から読んだ 1 ログ行を **CR/LF を取り除いた
状態** で送る。サーバーはリング バッファ (直近 200 行) に保存し、
`/monitor` SSE の購読者全員に転送する。長さは 4096 byte で切られる
(超過時は末尾に `…[truncated]` 付加)。Monitor 機能を実装しないクライアント
は送らなくてよい。

### `pong`
```json
{ "type": "pong" }
```

## ライフサイクル

```
browser connect
       │
       │  ── hello ──>
       │
       │  <── ping ── (every 15 s)
       │  ── pong ──>
       │
   (POST /flash arrives at server)
       │  <── flash_request ──
       │  <── [binary section #0] ──
       │  <── [binary section #1] ──
       │  <── [binary section #2] ──
       │
       │  (browser drives esptool-js)
       │  ── progress (×N) ──>
       │  ── log (×N) ──>
       │
       │  ── done {success:true} ──>
       │
   (POST /reset arrives at server)
       │  <── reset ──
       │  (browser toggles DTR/RTS via WebSerial)
       │
browser disconnect
```

## `/monitor` SSE (HTTP)

WebSocket とは別系統。任意のクライアントが
```
GET /monitor
Accept: text/event-stream
```
で購読すると、まず ` : monitor stream open` のコメント フレームと、
直近 200 行のリングを `data: <行>\n\n` で再生し、以降ブラウザが
`serial` フレームで push してくる 1 行ごとに同形式で配信する。
購読者は同時に複数 OK (fan-out)。サーバー側がブラウザ未接続でも
購読自体は成立する (何も流れないだけ)。

CLI ラッパは `tools/remote-flasher/monitor-stream.sh`。

## エラー処理ガイド (browser 実装者向け)

- WebSerial が掴めていない / port が閉じている状態で `flash_request` が
  来たら、即 `done {success:false, error:"no serial port"}` を返すこと。
- 進行中に WebSocket が切れたら、esptool-js の状態をクリーンに片付けて
  WebSerial を close する。サーバーは再接続を待つ。
- 通信エラーで esptool-js が例外を投げたら、`log` で詳細を流し、`done
  {success:false, error:...}` で締める。サーバーが SSE 経由で curl の
  呼出側に返す。

## バージョニング

このプロトコルは v1。後方互換性のない変更が必要になったら、`hello` と
`flash_request` に `protocol: "v2"` を入れる予定。今は何も無いことが v1
を意味する。
