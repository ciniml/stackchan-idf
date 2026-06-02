<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# 作業記録 (JOURNAL)

stackchan-idf の機能追加・修正の記録。新しいエントリを上に追記する。

---

## 2026-06 — Takao base / Atom-nyan / dual-firmware リリース (v0.3.0)

このシーズンは「ハードウェア バリエーション対応」が主軸。CoreS3 単一前提の
ファームから、CoreS3 + M5 base / Takao base / AtomS3R + Atomic ECHO BASE の
3 ボードを単一リポジトリでビルド・配布できる状態にした。

### サーボ原点/可動範囲の永続化 + 範囲設定モード

コミット: `47433ab`, `a9c0792`, `a88a887`

- 自作版「Takao Base + CoreS3 SE」対応。`enum BoardKind { M5Base, TakaoBase }`
  を `components/board/` に追加し、起動時に PY32 IO Expander (0x6F) を probe して
  検出 → 無ければ Takao。Takao 用に `Board::servo_bus_config()` を抽象化
  (M5: G6/G7 push-pull、Takao: G2/G1 + ダイオード経由半二重、echo_cancel)。
- **Takao 配線修正の罠**: 当初 TX=G1 / RX=G2 として書き込みは届いていた (頭は動く)
  が、サーボ応答が読めず常に `transact` が Timeout。波形観測の結果 `TX=G2 / RX=G1` が
  正しいと判明。書き込みだけ動いて読み戻しが silent fail する状態は気づきにくい。
- `ServoLimits` (yaw_zero / yaw_min_deg / yaw_max_deg / pitch_* の 6 つ) を
  NVS に保存できるよう DeviceConfig + BLE chr (`e3f0a018`) + Wi-Fi
  `/api/servo-limits` を追加。CoreS3 と Takao で物理的な取り付け方向が違うので。
- **範囲設定モード**: BLE / Wi-Fi / 本体タッチ UI どこからでも起動できる。
  ON にするとサーボのトルクが切れて手で頭を動かせるようになり、150ms 周期で
  `read_present_position` した値を BLE/HTTP で公開。capture ボタンで現在位置を
  zero/min/max として取り込める。本体 UI は 5 番目のタブ「範囲」として追加し、
  タブを離れると自動 OFF。

### LED ストリップ制御 — 一旦保留

コミット: `323df46`, `f5fa471`

- M5 base 背面の 12 × WS2812 を PY32 経由で制御するつもりで `Py32Expander` に
  `set_led_count` / `write_led_colors` / `refresh_leds` を追加し、`LedStrip`
  抽象 (board) + `led_task` (呼吸 / 単色 / レインボー アニメ) を実装。
- **判明した問題**: M5 BSP がドキュメントする `REG_LED_CFG=0x24` / `REG_LED_RAM_START=0x30`
  に書き込んでも実機の LED が一切反応せず、上位ビットを probe したら CoreS3 の LCD
  バックライトが消えた (リセットしても戻らず別要因と判明したが危険)。0x6F の PY32
  自体はサーボ電源 EN は正しく動いており、LED 制御の register map だけが
  BSP の想定と違うらしい。実機 firmware の正確なマップ判明まで `led->begin()` も
  `start_led_task` も呼ばない (API + コードは温存)。
- Phase 2 候補。

### アトムニャン (AtomS3R + Atomic ECHO BASE) 対応 — 画面 + 音声

コミット: `f2d97ab`, `6a1760d`

- `BoardKind::AtomNyan` 追加。`M5.begin` 完了後の `M5.getBoard()` が
  `board_M5AtomS3R*` を返したら即 AtomNyan で確定 (PY32 / Si12T probe をスキップ)。
- **音声**: M5Unified の `cfg.external_speaker.atomic_echo = 1` フラグを常時セット
  (CoreS3 系の case は触れないので無害)。これだけで AtomS3R 系では ES8311 codec の
  I2C/I2S 初期化 (BCK=G8 / WS=G6 / DIN=G7 / DOUT=G5) が自動。`M5.Mic.record` /
  `M5.Speaker.playRaw` 抽象に乗っているので会話タスクや録音テストは無改変で動く。
- **画面**: 320x240 ハードコードを実行時 `display.width()/height()` ベースに置換
  (render_task / avatar / balloon)。avatar は tick() で canvas 寸法変化を検出して
  `build_face` を再構築 (`set_face_tuning` は tuning だけ保持)。balloon は
  canvas 高 ≤160 px で 12-px フォント + 22-px パネル、それ以上で従来の 24-px /
  40-px。face.cpp の kBaseW/kBaseH = 320/240 はデザイン基準としてそのまま残置
  (uniform scale 機構が実行時 canvas へ縮小、AtomS3R は 0.4×)。
- **画面回転 (AtomS3R)**: 試行錯誤で `setRotation(0)` で正しい向き
  (CoreS3 は従来通り `setRotation(1)`)。`is_atom_s3r` 分岐の中で行うので
  CoreS3 への回帰なし。
- **device_ui (CoreS3 の 5 タブ設定 UI) は 128x128 に収まらない**ので、AtomS3R 用に
  最小ステータス画面 `main/atom_status.{hpp,cpp}` を新規追加 (USER_BUT で
  トグル、FW/SSID/IP/Wi-Fi/BLE/会話状態を 12-px フォント表示)。`render_task` は
  `ui::active() || atom_status::active()` で排他描画。
- AtomNyan ではサーボ電源/servo_task/LCD touch 処理をスキップ。demo_loop の
  ランダム姿勢・バブル・表情変化はそのまま動かす (yaw/pitch atomic はサーボ無しで
  無害、口/表情/バルーンはアバターに反映されて生きてる感が出る)。

### Multi-board ビルドシステム (BOARD 変数)

コミット: `6a1760d`

PSRAM モード (Quad / Octal) と flash 容量 (16MB / 8MB) は **bootloader が起動前に
確定する**ため runtime 切替不可。CoreS3 は ESP32-S3R8 (Quad / 16MB)、AtomS3R は
ESP32-S3-PICO-1-N8R8 (Octal / 8MB) で互換性なし → 別 firmware が必要。

- `make build BOARD=cores3` (default、後方互換) → `build-cores3/`
- `make build BOARD=atoms3r` → `build-atoms3r/`
- `sdkconfig.defaults.cores3` (Quad PSRAM + 16MB flash) / `sdkconfig.defaults.atoms3r`
  (Octal + 8MB) を per-board overlay として SDKCONFIG_DEFAULTS チェーンに追加。
  `sdkconfig.defaults.esp32s3` は flash size / PSRAM mode を持たない共通設定に。
- `partitions.csv` (0x710000 = 7.5MB 利用) は両方に収まるので変更なし。
- `.gitignore` を `build-*/` パターンに拡張。

### 設定 UI のボード種別表示 + 自動ゲート

コミット: `5841357`, `2e75968`

- ファームに `set_board_kind(uint8_t)` API を追加 (config_service + wifi_config)。
  BLE 側は新しい R-only 暗号化 chr (`e3f0a01b`)、Wi-Fi 側は `/api/status` の
  `board` フィールド。値は `BoardKind` cast (0=M5Base, 1=TakaoBase, 2=AtomNyan)。
- BLE 設定ページ (`tools/settings.html`): 既存の DIS Model 行を上書きして
  ボード種別 (「CoreS3 + M5 base」/「AtomS3R + Atomic ECHO BASE」等) を表示。
  AtomNyan では `#servo-section` を `display:none`。バッテリー無 (Takao /
  AtomNyan) ではバッテリー行 + バッテリーゲージトグルも非表示。
- Wi-Fi 設定ページ (`settings_wifi.html`): DIS Model 行が無いので新規「基板」行を
  追加。`refresh()` で `applyBoardGating(kind)` 呼び出し。
- 古い firmware (BoardKind 公開なし) は boardKind=null → 全表示 = 従来通り
  (回帰なし)。
- スマホでアバター調整スライダーが指で操作不能だった件: `#avatar-section .av-row` に
  `@media (max-width:540px)` で flex-wrap、ラベルを 100% 幅にして強制 1 行目に
  独立 → スライダー + 値を 2 行目に。デスクトップ レイアウトはそのまま。

### demo_loop の Wi-Fi 待ちスキップ (会話無効時)

コミット: `b3fe005`

「Wi-Fi: 切断中」バブル + バブル抑制は会話バックエンド (OpenAI / Gemini /
XiaoZhi) を使うときだけ意味がある。`cfg.openai_enabled = false` のときは
ローカル jtts のみで完結するので、`wifi_ok` を常に true として扱って即座に
アイドル動作を開始する。

注: `wifi_is_connected()` は単なる observer (atomic bool)。`wifi_start()` も
`wifi_config::start()` も会話有効/無効に関係なく起動するので、**Wi-Fi 経由の設定
UI / mDNS / wifi_audio は会話オフでも普通に動く** (デモループ内の
表示制御だけが変わる)。

### Dual-firmware リリース パイプライン

コミット: `9dea018` (リリース: **v0.3.0**)

- `script/pack_firmware.py` に `--build-dir` / `--out` 引数追加。
- `.github/workflows/release.yml` を `strategy.matrix.board=[cores3, atoms3r]` に。
  各 leg は `make build BOARD=$BOARD` → `firmware-vX.Y.Z-cores3.zip` /
  `firmware-vX.Y.Z-atoms3r.zip` を同じ Release にアタッチ
  (softprops/action-gh-release はタグ単位で merge)。`fail-fast: false` で
  片方コケても他方を出す。
- `.github/workflows/pages.yml`: 各タグの全 `firmware-*.zip` を staging し、
  `versions.json` を `[{tag, boards: {cores3?, atoms3r?}}, ...]` 形式に変更。
  filename suffix で board 判別、suffix なし (v0.2.17 以前) は cores3 にマップ。
- `docs/index.html` (Web フラッシャ) に Board セレクタ追加。`zipFor(rel, board)`
  で `boards.<board>` → 旧 `.zip` → null の順に解決。該当 firmware が無い
  リリース選択時は Flash ボタン無効化 + 警告。
- レガシー版 (v0.2.17 以前) は CoreS3 のみフラッシュ可能の動作で回帰なし。

### v0.3.0 リリース

- Tag: v0.3.0 push 済み、両ボード matrix build 成功 (`App "stackchan_idf"
  version: v0.3.0` clean、`-dirty` なし)。
- CoreS3 / アトムニャン両機種で実機検証完了 — 起動 / アバター / 音声 (会話含む)
  すべて動作確認済み。
- Live: <https://ciniml.github.io/stackchan-idf/> から両ボード向け ZIP を
  選択フラッシュ可能。
- リリースは `.claude/skills/release/SKILL.md` の手順に準拠 (tag push → matrix
  build → 手動で `gh workflow run pages.yml` → smoke-test)。pages.yml は
  リリース published イベントでも triggers されるが、`GITHUB_TOKEN` 由来の
  Release 作成では発火しないため手動 dispatch が必要 (既知 / SKILL に記載済み)。

---

## このシーズンで持ち越した課題

- **CoreS3 LCD バックライト**: PY32 LED probe 中に消えて電源再投入後も復旧
  しなかった件 (`f5fa471` のコミットメッセージ参照)。後に「別要因」と判明したが
  根本原因は未解明。要調査タスク。
- **M5 base 背面 NeoPixel**: 上記理由で PY32 LED 制御は完全に無効化中。実機
  firmware の正しい register map (BSP の `0x24/0x30` は外れ) が判明したら
  `Board::begin()` 内の `led->begin()` と `app_main` の `start_led_task()` の
  2 行を戻すだけで再開できる構造。
- **AtomS3R Phase 2 候補**: Grove ポート (G1/G2) 経由のサーボ駆動、USER_BUT
  長押し/ダブルタップ、AtomS3R 用 face preset (0.4× スケールで表情アニメが
  steppy な場合の見栄え改善)。

---

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
