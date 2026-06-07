<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# stackchan-channel — Claude Code Channel adapter for Stack-chan

Claude Code から Stack-chan (M5Stack の卓上ロボット firmware) を **MCP tool
として直接操作**するための Bun TypeScript アダプタ。

Stack-chan firmware の `/mcp/*` REST API ([components/wifi_config_service/http_handlers.cpp](../../components/wifi_config_service/http_handlers.cpp))
を Claude Code Channel (`claude/channel` 機能) でラップする。Claude が
`mcp__stackchan__say` 等を呼ぶ → このアダプタが Stack-chan の HTTPS REST に
転送 → 実機が即時反応。

## アーキテクチャ

```
[Claude Code (どこか)]                  [自宅 LAN]
   │ stdio MCP                       ┌──────────────────┐
   ▼                                 │ cloudflared      │
┌──────────────┐  HTTPS              │ stackchan.user.  │
│ stackchan-   │─── + Bearer ────────│ example.com      │
│ channel      │     (Tunnel経由)    │                  │
│ (this Bun)   │                     │ ↓ LAN HTTP       │
└──────────────┘                     │ Stack-chan       │
   │ subprocess                      │  esp_http_server │
   ▼                                 │  + /mcp/*        │
[claude]                             └──────────────────┘
```

## 提供される MCP tool

| Tool | 説明 |
|---|---|
| `say(text)` | 発話。text は ひらがな/カタカナ (jtts は漢字 → 読み変換 を持たない) |
| `set_expression(expression)` | `neutral` / `happy` / `sad` / `angry` / `doubt` / `sleepy` |
| `set_balloon(text, hold_ms?)` | アバター下の吹き出しに表示 |
| `get_state()` | FW ver / IP / Wi-Fi / バッテリ / ボード種別を JSON で取得 |

## push events (Phase 2 〜)

device 側で起きたことを Claude に通知する仕組み。adapter が起動時に
`GET /mcp/events` を long-poll (SSE) で開き続け、firmware が発生したイベントを
`text/event-stream` で順次 push。adapter は MCP の
`notifications/claude/channel/event` として Claude に転送する。

| event | data 例 | 発火タイミング |
|---|---|---|
| `boot` | `{"firmware":"v0.5.2","ip":"192.168.1.42","board":0}` | Wi-Fi で IP を取得した直後に 1 回 |
| `touch` | `{"direction":"front_to_back"}` | 頭部 capacitive touch で front→back / back→front のストロークが完了 |
| `say_done` | `{}` | `say` で渡したテキストの再生が物理的に終了 (speaker queue drain 後) |
| `conversation_state` | `{"state":"listening"}` | 会話バックエンドの状態遷移 (`disabled`/`waiting_wifi`/`connecting`/`listening`/`talking`/`yielded`/`reconnecting`/`error`) |

wire 上は `event: <type>\ndata: {…}\n\n` の素直な SSE 形式。15 秒ごとに
`: keepalive` コメント フレームが入り、Cloudflare Tunnel の idle close を
回避する。adapter 側は切断時に exponential backoff (1s→30s) で自動再接続。

## セットアップ

### 1. Stack-chan firmware 側

`sdkconfig.defaults.local` (gitignore 済) に Bearer token を設定:
```
CONFIG_MCP_API_TOKEN="ランダムな英数字 32 文字程度"
```
空文字 = `/mcp/*` 全て 404 で disable される。

`make build flash` で焼き込み。

### 2. Cloudflare Tunnel (自宅サーバから Stack-chan を公開)

`cloudflared` を Stack-chan と同じ LAN の常時起動マシン (Mac mini / Raspberry
Pi / NAS 等) にインストール:

```bash
# Zero Trust ダッシュボードで Tunnel 作成 → 認証情報 (JSON) ダウンロード
cloudflared tunnel create stackchan
cloudflared tunnel route dns stackchan stackchan.user.example.com

# Public Hostname として stackchan.user.example.com → http://192.168.x.x:80
# (Stack-chan の LAN IP) を設定

cloudflared tunnel run stackchan
```

systemd / launchd / Docker 等で常駐化推奨。

### 3. Claude Code 側

`tools/stackchan-channel/` で依存をインストール:
```bash
cd tools/stackchan-channel
bun install
```

プロジェクト root に `.mcp.json` を置く (or 既存のものに追記):
```json
{
  "mcpServers": {
    "stackchan": {
      "command": "bun",
      "args": ["./tools/stackchan-channel/index.ts"],
      "env": {
        "STACKCHAN_URL": "https://stackchan.user.example.com",
        "STACKCHAN_TOKEN": "ランダムな英数字 32 文字程度"
      }
    }
  }
}
```

`STACKCHAN_TOKEN` は firmware 側 `CONFIG_MCP_API_TOKEN` と完全一致させる。

リサーチ プレビュー期間中、自前 Channel は allowlist に無いので
`--dangerously-load-development-channels` で起動:
```bash
claude --dangerously-load-development-channels server:stackchan
```

## 動作確認

### Channel adapter なしで firmware だけ叩く

```bash
# /mcp/state — 状態取得
curl -H "Authorization: Bearer $STACKCHAN_TOKEN" \
     https://stackchan.user.example.com/mcp/state

# /mcp/say — 発話 (text/plain body)
curl -X POST -H "Authorization: Bearer $STACKCHAN_TOKEN" \
     -d "こんにちは" \
     https://stackchan.user.example.com/mcp/say

# /mcp/expression
curl -X POST -H "Authorization: Bearer $STACKCHAN_TOKEN" \
     -d "happy" \
     https://stackchan.user.example.com/mcp/expression

# /mcp/balloon (hold_ms はクエリ パラメタ)
curl -X POST -H "Authorization: Bearer $STACKCHAN_TOKEN" \
     -d "Hello!" \
     "https://stackchan.user.example.com/mcp/balloon?hold_ms=3000"
```

### Claude Code 経由

`claude --dangerously-load-development-channels server:stackchan` で起動後、
terminal で:
```
> Stack-chan に「こんにちは」と言わせて
```
→ Claude が `mcp__stackchan__say(text="こんにちは")` を呼んで実機が発話。

## 想定エラー

| ステータス | 原因 / 対策 |
|---|---|
| `HTTP 404` | `CONFIG_MCP_API_TOKEN` が空 → firmware が API を disable。`sdkconfig.defaults.local` で設定して再ビルド |
| `HTTP 401` | `STACKCHAN_TOKEN` 不一致。adapter env と firmware Kconfig を見比べる |
| `HTTP 503 service unavailable: ... sink not registered` | firmware 起動シーケンス中 (起動直後)。app_main の `set_mcp_*_sink` が走るまで待つ |
| `fetch error: ENOTFOUND` | Cloudflare Tunnel が落ちてる、または DNS 未伝播 |
| 発話されない (say は 200 を返すが音が出ない) | 別の音声が再生中、または ECHO BASE 未接続 (AtomS3R) |

## ファイル

- `index.ts` — メイン (~150 行)
- `package.json`
- `README.md` (このファイル)

## /mcp/events を手元で見る (デバッグ)

```bash
curl -N -H "Authorization: Bearer $STACKCHAN_TOKEN" \
     https://stackchan.user.example.com/mcp/events
# 起動直後:
# : stackchan mcp-events stream open
#
# event: boot
# data: {"firmware":"v0.5.2","ip":"192.168.1.42","board":0}
#
# event: conversation_state
# data: {"state":"waiting_wifi"}
#
# ... (なで操作で)
# event: touch
# data: {"direction":"front_to_back"}
#
# : keepalive   (15 秒ごと)
```

同時 subscriber は 1 つだけ。新しい GET が来ると以前の長期接続は切断される
(adapter を多重起動した場合の症状)。

## 制限事項

- **テキスト → 漢字 → 読み 変換 なし**: 漢字混じり文字列は jtts が読めない。
  Claude には「ひらがな/カタカナで指示せよ」を `instructions` で伝達済
- **同時 say の調停 粗い**: 2 つの `say` が連続すると 2 つ目のワーカが
  speaker idle 待ちでブロック。長時間連投すると task heap が嵩む。今は許容
- **`notifications/claude/channel/event` は experimental**: Claude Code の
  Channel 機能はリサーチ プレビュー。schema 変更があり得るので、見えなく
  なったら adapter の method 名を見直す
- **bearer token は Kconfig**: BLE / Wi-Fi 設定 UI からは変更不可。Phase 3
  で NVS 化予定
