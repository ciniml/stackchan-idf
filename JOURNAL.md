<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# 作業記録 (JOURNAL)

stackchan-idf の機能追加・修正の記録。新しいエントリを上に追記する。

---

## 2026-05 — なでなで応答 + OpenAI Realtime API 音声対話

### なでなで応答 (本体上部タッチセンサー)

コミット: `204e8b2`

- 本体上部の Si12T 静電容量タッチ IC (内部 I²C `0x68`、Front/Middle/Back の 3 ゾーン、各 2bit 強度) のドライバを `components/board/` に追加 (`si12t_touch.{hpp,cpp}`)。初期化シーケンスは StackChan-BSP の Si12T ドライバを参照し、データシート (`Si12T_Datasheet_EN.pdf`) でレジスタマップを検証。
- `Board` に `touch_sensor()` アクセサを追加し、`Board::begin()` で probe (チップ未実装の旧基板でも起動するよう warn-only)。
- `demo_loop` で 50ms 周期に Si12T をポーリングし、いずれかのゾーンが 250ms 以上連続接触されたら「なでなで反応」を発火: `Happy` 表情 + 「なでなで♡」バルーン + ヨー ±8° を 4 周期高速振動 (`servo_speed_override` で約 120°/s)。4 秒のクールダウン付き。
- `servo_task` が `SharedState::servo_speed_override` を一時的な Goal Speed として使うよう変更。

### OpenAI Realtime API 音声対話 — 基本実装

コミット: `fa811c9`

- 汎用インターフェース `components/conversation/conversation_service.hpp`: AI 対話サービスを backend 非依存で扱う `ConversationService` 抽象クラス。`ConversationEvent` (状態変化 / ユーザー transcript / アシスタントテキスト / 音声チャンク / ツール呼び出し / エラーを 1 つの struct で表現)、`ConversationConfig`、`ToolDefinition`。音声は `shared_ptr<const vector<int16_t>>` で運び、FreeRTOS キュー越しのコピーを refcount のみに抑える。
- `OpenAiRealtimeClient`: `esp_websocket_client` (Component Registry の managed component) で `wss://api.openai.com/v1/realtime` に接続。cert bundle + `Authorization`/`OpenAI-Beta` ヘッダ、PSRAM 上のフレーム再構成、cJSON で全 server event をパース。`input_audio_buffer.append` はホットパスとして snprintf + mbedtls base64 で per-call ヒープ確保ゼロ。`session.update` / tool result は cJSON。送信は `send_mutex_` で直列化。
- アプリ コーディネータ `main/conversation_task.cpp`: 半二重 I2S 状態機械 (`Init→Listening→Thinking→Speaking`)。マイクを 40ms チャンクでダブルバッファ ストリーム、口パク同期、transcript をバルーン表示、ツール `set_expression` / `set_head_pose` をエンドツーエンドで実装。
- 起動後 Wi-Fi 接続で自動的に常時リッスン開始。会話中は `demo_loop` を停止。LCD タップの AAC デモは削除。
- API キーは Kconfig 文字列 (`CONFIG_STACKCHAN_OPENAI_API_KEY`) を gitignore 済みの `sdkconfig.defaults.local` に記述。

**ハード制約:** CoreS3 のマイク (PDM) とスピーカー (標準 I2S) は `I2S_NUM_1` を共有し `GPIO34` も共用。同時動作不可 → 対話は半二重 (聞く→話す→聞く)。

### 音声途切れの修正

コミット: `0b4a5f8`

会話時の応答音声が途切れる問題を調査。原因は 2 つ:
- M5Unified のマイク/スピーカー I2S タスクが優先度 2 (render/conversation/servo/WebSocket タスクより下) で starve していた → 優先度 6 に引き上げ、スピーカー DMA バッファを倍増。
- `M5.Speaker.playRaw` はバッファをコピーせず参照し、リサンプラがサンプル単位で読む。応答が PSRAM 上にあると render タスクの 30fps スプライト転送と PSRAM 帯域を取り合い I2S DMA が underrun。→ 応答は PSRAM に蓄積したまま、再生は ~341ms (後に ~512ms) のセグメント単位で内部 RAM のリングバッファ (3 枚) にコピーしながら `M5.Speaker` に流す方式に変更。スピーカーは常に高速 SRAM だけを読む。

### 応答コーデックを G.711 µ-law に

コミット: `8bc0f4d`

- `output_audio_format` を `g711_ulaw` (8kHz) に変更。コーデック差は `OpenAiRealtimeClient` 内に閉じ込め (µ-law → PCM16 を内部デコード)、汎用インターフェースは PCM16 のまま。
- 切替点は `ConversationConfig.output_sample_rate_hz` (8000 → g711_ulaw、24000 → pcm16。OpenAI は pcm16@24k と g711@8k しか無いのでレートが一意にコーデックを決める)。
- マイク入力は pcm16 24kHz を維持。`conversation_task` はマイク (24kHz) とスピーカー (8kHz) のレートを分離。

### ストリーミング再生 + タッチ割り込み (barge-in)

コミット: `f1b93cd`

- **ストリーミング再生:** 応答全体を待たず、~300ms のジッタバッファが貯まり次第発話開始。応答バッファは再生中も成長し続け、セグメントリングがそこからストリーム。口パクエンベロープは再生中の窓からオンザフライで算出。
- **タッチ割り込み:** 音声での barge-in は半二重ハードウェア上不可能なので、応答再生中に Si12T 頭部センサーを触ると割り込み → 再生停止 + `response.cancel` + リッスン復帰。`ConversationService` に `cancel_response()` を追加。
- `response.cancel` は実際に応答生成中のときだけ送信 (`response_active_` フラグ)。サーバはリアルタイムより速く応答を送り終えるので、barge-in 時点では大抵もう生成完了 → 以前は毎回 "no active response" サーバエラーになっていた。
- サーバ側エラーは非致命的扱い (ログのみ)。全 WebSocket 再接続を起こすのはトランスポートエラー (切断/ハンドシェイク失敗) のときだけ。

### 待機中の demo 挙動を会話中も復活

コミット: `485d56a`

常時リッスンで `conversation_active` が永続 true になり `demo_loop` が完全停止 → 待機中もスタックチャンが動かなくなっていた。
- 会話タスクが `Local` 状態を `SharedState.conversation_idle` に公開 (`Listening` のときだけ true)。
- `demo_loop` を 3 段階に整理:
  - 会話なし → フル demo (首振り・なでなで・babble・表情サイクル・口パク・Wi-Fi balloon)
  - 会話中・待機リッスン中 → アイドル demo のみ (ランダム首振り + なでなで反応)
  - 会話中・思考/発話中 → 全停止 (会話タスクがアバターを占有)
- 発話中に頭を触れば従来どおり barge-in。

---

## 既知の改善余地

- **会話コンテキストの正確性:** barge-in 後もサーバ側には「応答を最後まで言った」と記録される。`conversation.item.truncate` (item_id と再生済み ms をサーバに伝える) が必要。
- **セッション切断時の自動再接続** の洗練。
- **マイク入力品質:** 最初の発話が稀に誤認識される。
- **`m5::In_I2C` の並行アクセス:** demo_loop (M5.update + なでなでポーリング) と conversation_task (barge-in ポーリング) が状態遷移の隙間で内部 I²C に同時アクセスし得る。実害は出ていないが、必要なら mutex 化。

## 環境メモ

- 書き込みポート: スタックチャン本体は `/dev/ttyACM0`。`idf.py` の自動検出は別ポート (`/dev/ttyACM1`) を選ぶことがあるため、`make flash PORT=/dev/ttyACM0` でポートを明示すること (または `Makefile.local` に `PORT = /dev/ttyACM0`)。
