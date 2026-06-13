<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# stackchan remote flasher — browser side

VPN 越しのリモート ブラウザで実機 (CoreS3) に **WebSerial + esptool-js** で
flash を書き込むためのフロント エンド。host 上の relay server (別エージェント
担当) とは WebSocket で繋ぎ、書き込みバイナリは server から push される。

このディレクトリ (`tools/remote-flasher/web/`) はブラウザ側だけを扱う。
サーバ実装は `tools/remote-flasher/` 配下の別ディレクトリに置かれる予定。

## ビルド

```sh
cd tools/remote-flasher/web
bun install
bun run build
```

`bun run build` は `src/esptool-entry.js` を `esptool-bundle.js` (IIFE, minify
済み) にまとめ、`window.EsptoolJS = { ESPLoader, Transport }` を公開する。
ビルド成果物はリポジトリにコミットしない (`.gitignore` の対象だが、配布する
場合は手動で同梱する)。

依存:

- `esptool-js` (Espressif 公式 — https://github.com/espressif/esptool-js)
- ランタイムでは外部 CDN を一切叩かない。完全オフライン動作。

## 起動

`index.html` と `esptool-bundle.js` を同一オリジンで配信する。relay server が
HTTP も担当する想定 (例えば `http://localhost:8080/` で `index.html`、
`ws://localhost:8080/ws` で WebSocket)。

ローカル確認だけしたい場合:

```sh
cd tools/remote-flasher/web
python3 -m http.server 8080
# http://localhost:8080/ をブラウザで開く
```

## 必須環境

- **ブラウザ**: WebSerial API 対応のもの (Chrome / Edge / Opera など Chromium
  系の最新版)。Firefox / Safari は非対応で、起動時にエラー バナーを出す。
- **HTTPS or localhost**: WebSerial は secure context でしか使えない。
  - 本番運用 (VPN 越しのリモート) では HTTPS で配信する。自己署名でも可。
  - 開発時は `http://localhost:*` / `http://127.0.0.1:*` は例外で動く。
- **OS 側のシリアル権限**: Linux は `dialout` グループに参加 (もしくは
  `chrome://device-log` で権限を確認)。Windows は標準で OK。macOS も標準で OK。

## 使い方

1. ブラウザでページを開く。WebSerial 未対応ブラウザは赤バナーが出て機能停止。
2. **[シリアル ポート選択]** を押し、USB-CDC デバイス (CoreS3) を選ぶ。
   - 自動で `ESPLoader.main()` を実行してチップ種別を判別し、設定の "ESP
     チップ" を書き換える。
   - 同期に失敗した場合は基板の BOOT を押しながら EN/RST を押し、もう一度
     ポートを選び直す (ガイダンスをログに出す)。
3. **[接続]** で WebSocket を開く。`hello` を送って relay server に名乗る。
4. server から `flash_request` JSON + section ごとのバイナリ frame が来ると、
   全部揃った時点で `ESPLoader.writeFlash()` を一括で実行し、進捗を
   `progress` メッセージで返す。終了で `done` を返す。
5. **[リセット]** はいつでも押せる。DTR/RTS を叩いて再起動する。
6. **[切断]** で WS を閉じる。明示切断時は自動再接続しない (それ以外は 5 秒で
   再接続)。

## プロトコル

`tools/remote-flasher/PROTOCOL.md` が **正本**。本実装はそれに従う:

- `progress.section` は **セクション名 (`flash_request.sections[i].name`)**
  であり、index ではない。
- `hello` には `chip` を含めない (relay server が決める)。
- `superseded` を受けたら自動再接続せず、静かに WS を閉じる。
- WebSerial で port を掴んでいない状態で `flash_request` を受けたら、
  即 `done {success:false, error:"no serial port"}` を返す。

サーバの WS デフォルトは `ws://<host>:8765/ws`。`index.html` の "サーバー
WebSocket URL" フィールドは現在のページ オリジン (`/ws` パス) を初期値に
してあるので、relay server を別ポートで動かす場合は手動で書き換える。

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| 起動時に赤バナー "WebSerial API 非対応" | Chrome / Edge を使う |
| `requestPort` で「キャンセル」のエラー | ダイアログでデバイスを選ぶ必要あり (user gesture 必須) |
| `ESPLoader.main()` が失敗する | BOOT 押下しながら EN/RST → 再度 [シリアル ポート選択] |
| WS が即切断される | server の URL / TLS / 認証を確認。`ws-url` を編集して再 [接続] |
| flash 後に monitor で文字化け | ボーレートが書込み用 (460800) のまま。`[リセット]` を押すと既定で 115200 に戻る |

## 既知の制限

- ESP32-S3 以外への対応は relay server からの `chip` 指定に依存する。
  ブラウザ側は esptool-js の自動判別を信用する。
- 1 セッション内で書き込みが直列に走る前提 (`busy` 応答で多重要求を弾く)。
- `flash_request` のバイナリ frame 受信タイムアウトは未実装。relay server 側
  でタイムアウトと再送を扱う。
