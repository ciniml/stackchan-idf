<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# 作業記録 (JOURNAL)

stackchan-idf の機能追加・修正の記録。新しいエントリを上に追記する。

---

## 2026-06 (下旬) — aokko 顔 / 会話音声品質 / ネコミミ LED / AtomS3 slim

このシーズンは「ハードウェア バリエーション + 設定 UI 拡張」が主軸。
リリース v0.4.1 → v0.4.5 → v0.5.0 と細かく刻みつつ、avatar DSL の新
プリセット (aokko)、会話音声の周波数ドリフト修正、ネコミミ NeoPixel
LED の追加 (CoreS3 + AtomNyan + 新 AtomS3 slim)、設定 UI に LED + 音声
diagnostics パネル、を入れた。

### aokko-style 顔プリセット (avatar DSL P1)

コミット: `9026686` (preset 追加), `c74a05f` (まつげ統合)

- `assets/aokko_face.avdsl` を新規追加。`mongonta0716/m5stack-avatar-mic-
  nekomimi_led` の `aokkoface` ブランチ (MIT 2024 washishi) の見た目を
  既存 AVDS VM (Lua 風 DSL → バイトコード) で再実装。座標 / 色のような
  事実情報のみ引用、コードは持ち込まずに書き起こした (BSL-1.0 単独で
  配布可)。
- 部品: 縦長 pill 形の目 (矩形 + 上下端 fill_circle、内部で奇数スナップ
  して矩形と円のピクセル幅を 1px 単位で一致)、六芒星の頬、横長口、
  眉なし、表情エフェクトなし。HAPPY 時のみ目の下半分マスクで `⌒` 形に。
- ネコミミの見た目に合わせて「3 本まつげ」を ellipse_eye 内に統合
  (gaze_h/v で目玉と一緒に動く、open 時は ⌒-fan / close 時は `|||`)。
- WASM プレビュー (`wasm/build.sh`) に `aokko` プリセット ボタンを追加。
- 持ち越し: aokko の WiFi 経由プリセット一覧に未登録の件は、
  components/wifi_config_service/web/settings_wifi.html 側の更新が必要
  (settings.html / WASM は対応済)。

### 会話音声: ターン跨ぎの周波数ドリフト修正

コミット: `3051e6a` (m5u patch)

- 症状: 会話ターンを重ねると応答音声が徐々に遅くなる (周波数が下がる +
  ノイズ感)。リブートまで回復しない。CoreS3 では Mic/Speaker が
  I2S_NUM_1 を共有し毎ターン swap するので、M5Unified の Speaker_Class::
  end() が `_ch_info[ch].diff` (フラクショナル累算器) / `index` / `flip`
  / `liner_buf[2][2]` (線形補間 2 サンプル履歴) を保持したまま終了し、
  次 begin() で中途半端な値から再開していた。
- 修正: `patches/m5unified.patch` に Speaker_Class::end() の追加リセット
  (diff/index/flip/liner_buf を 0 クリア) を当てる。実機検証でターンを
  重ねても周波数低下が起きないことを確認。

### 会話音声パイプラインの計装と外部公開

コミット: `fd8af45` (5 指標), `86e9136` (played_sps), `8eeb17a`
(BLE / HTTP 公開), `d1b47ea` (settings.html パネル)

- ターン毎に ESP_LOGI で 1 行出力する音声パイプライン統計を追加:
  decode_us (base64 + memcpy), recv_lag_us (event queue 渡し), recv_to_
  queued_ms (受信から playRaw 投入までの遅延), spk_queue 深度 (0..2),
  pcm_lag_samples (未再生サンプル数), played_sps (実効再生レート)。
- `Stats<T>` テンプレートを `components/conversation/include/conversation/
  metrics.hpp` に共有、ConversationEvent に `emit_us` フィールド追加。
- `SharedState::AudioMetrics` snapshot を mutex 保護で公開、HTTP `GET
  /api/metrics/audio` と BLE chr 0x1f (encrypted READ JSON) 両方から
  取得可能に。
- `tools/settings.html` に「音声パイプライン診断」パネル追加 (表組 +
  自動更新 2 秒)。played_sps / nominal の比が < 99% なら赤強調。
- 実測結果: ターン跨ぎドリフトは patch 3051e6a で解消。残った played_sps
  ≈ 23790/24000 (~99.1%) は I2S MCLK 整数分周由来で実用許容。「ターン
  累積で遅延」と感じていた症状の一部は Telegram クライアントの影響だと
  判明、無効化で解消。
- 持ち越し: I2S MCLK 0.9% slow、spk_task stack HWM 300 B 切迫、INT
  min ~67 B の慢性タイト。

### ネコミミ NeoPixel LED 制御 (GPIO9 / GPIO38)

コミット: `45a4865` (ドライバ), `403d7e5` (BLE/HTTP API), `10aee3c`
(設定 UI), `d30dba0` (NVS 永続化), `abe6b5c` (虹周期), `df9c1c3`
(AtomNyan G38)

- Stack-chan のネコミミ部 WS2812 チェーン (左耳 9 + 右耳 9、18 LED) を
  espressif/led_strip 経由 RMT で駆動。CoreS3 系は GPIO9、AtomNyan
  (AtomS3R) は Atomic ECHO BASE 経由で GPIO38。
- LedStrip を抽象基底化、既存 PY32 実装を Py32LedStrip に rename、
  NekomimiLedStrip を新規追加 (RMT 10 MHz、48 mem symbol、no DMA)。
  Board::Impl::led_ を std::unique_ptr<LedStrip> 化。
- BLE encrypted R/W chr 0x20 + HTTP `GET/POST /api/led-state` で動的に
  mode (off/solid/breath/gradient) / color / brightness / 虹周期を
  操作可能に。BLE wire は 6 byte `[mode][R][G][B][bright][period_ds]`
  (5 byte legacy 受理、period 据置)。
- NVS 永続化: DeviceConfig に 4 keys 追加、`store::save_led_state()` を
  single writer に。フル `store::save()` から LED keys を撤去 (Apply
  経路が g_active の boot 値で runtime 変更を上書きする race を排除)。
- 設定 UI に LED セクション + 4 入力 (mode select / color picker /
  brightness + 虹周期 slider)、150 ms debounce で BLE 書き込み。
- 持ち越し: 左右ペア独立アニメーション (`kLedsPerEar = 9` を活用)、
  WiFi 設定 UI (settings_wifi.html) 側の LED パネル。

### AtomS3 (PSRAM 無し) 向け slim プロファイル

コミット: `e384ac7` (board / Kconfig), `c7f5823` (CI matrix / pages)

- PSRAM が無い AtomS3 (C014) + Atomic ECHO BASE で「avatar + jtts +
  ネコミミ LED + 管理 UI (BLE/HTTP 設定)」だけを動かすビルド ターゲット。
  会話タスク / BLE 音声ストリーミング / RTP 受信のような PSRAM 必須機能
  をコンパイル時に削減。
- 新 Kconfig フラグ 3 つ (CONVERSATION/AUDIO_STREAM/WIFI_AUDIO_ENABLED、
  default y、AtomS3 で n)。`main/Kconfig.projbuild` に menu 追加。
- 新 `sdkconfig.defaults.atoms3`: CONFIG_SPIRAM=n、8MB flash、3 機能 off、
  Wi-Fi/LWIP/mbedTLS バッファを SRAM 制約向けに縮小。
- BoardKind::AtomS3 追加。Board::begin() で `M5.getBoard() ==
  board_M5AtomS3` を検出し AtomS3R 系と統合した軽量 init パス
  (rotation(0) / PY32+Si12T probe スキップ / NekomimiLedStrip GPIO38)。
- main/CMakeLists.txt: conversation_task / audio_stream_sink / wifi_audio
  の SRCS を Kconfig 条件除外。PRIV_REQUIRES の conversation はそのまま
  (ESP-IDF 早期 expansion で Kconfig 未評価のため条件付きはダメ、dead-
  code elimination 任せ)。
- app_main.cpp の seg_buf alloc / start_X 呼出 / ConversationTaskArgs
  生成 / 関連 include を `#if CONFIG_*` で gate。AtomS3 を is_atom_nyan
  判定に含めて servo/UI 分岐共通化。
- render_task.cpp: CONFIG_SPIRAM gate で esp_psram_get_size 呼出を
  スキップ、direct canvas (partial update) に強制。
- CI: release.yml の matrix.board に atoms3 追加 (3 並列ビルド)、
  pages.yml classifier + docs/index.html ボード セレクタに対応。
- ビルド サイズ: cores3 / atoms3r ~2.65-2.67 MB、**atoms3 2.08 MB**
  (release ZIP は atoms3 2.89 MB)。
- 持ち越し: 実機検証 (AtomS3 ハードウェアまだ手元になし)。

### 会話タスク回りのこまごま修正

コミット: `8e48eb2` (AtomNyan led->begin), `ea11b7c` (NVS race)

- AtomNyan の Board::begin() が早期 return して `led->begin()` をスキップ
  していたため GPIO38 ネコミミ LED が点灯しなかった件を修正。
- LED 設定の NVS 永続化 race を `store::save()` からの LED keys 撤去で
  解消 (詳細は LED セクション)。

### remote flasher 拡張 (前シーズン継続)

コミット: `b1a8d85` (`/app/*` 静的配信), `1e08378` (シリアル モニタ),
`6d88a1f` (README), `3bdf71e` (port 再オープン バグ修正)

- 前シーズンの remote-flasher (Bun サーバ + WebSerial + esptool-js +
  curl ラッパ) に追加機能:
  - `/app/*` で Web UI をホストから直接配信 (CDN 不要)
  - `/monitor` SSE + `monitor-stream.sh` で idf.py monitor 相当
  - 2 回目 flash で "port already open" になる修正 (stopMonitor で
    reader cancel → ループ終了待ち → port.close まで完遂)

### リリース履歴

| Tag | 主な変更 |
|---|---|
| `v0.4.2` | 会話音声 patch + 音声 metrics 計装 + BLE/HTTP 公開 + UI |
| `v0.4.3` | ネコミミ LED CoreS3 (GPIO9) + LED 設定 + NVS 永続化 + 虹周期 |
| `v0.4.4` | AtomNyan ネコミミ LED 対応 (GPIO38) |
| `v0.4.5` | AtomNyan LED begin() 修正 + LED NVS race 修正 |
| `v0.5.0` | AtomS3 slim プロファイル (3 ボード matrix) |

### 持ち越し課題 (このシーズン分)

- **WiFi 設定 UI (`settings_wifi.html`) のアバター プリセットに aokko 未掲載**:
  WASM プレビュー (`wasm/build.sh`) と `tools/settings.html` には登録済
  だが、HTTP 側のプリセット選択肢に出ない。settings_wifi.html の更新が
  必要。
- **Avatar DSL 転送を BLE でも可能に**: 現状 `POST /api/avatar-dsl`
  (HTTP) のみ。BLE chr 経由でも `.avbc` を送り込めるようにしたい
  (Avatar 関連は WiFi / BLE 共通機能とする方針)。
- **AtomS3 実機検証**: ビルドのみ完了、ハードウェア未入手。
- **左右ペア独立アニメーション モード**: `NekomimiLedStrip::kLedsPerEar`
  を活用して L/R それぞれに別パターンを当てる。
- **I2S MCLK 0.9% slow / spk_task stack 300 B / INT min ~67 B**: 慢性
  メモリ + クロック精度の制約。新機能で問題が再燃したら個別対応。
- **mcp_events.cpp の stack 3→4 KiB**: カメラ無効化で発生条件は消えた
  ので未コミット保留 (今後のリリースで取り込み判断)。

---

## 2026-06 (中旬) — Claude Code Channel / LT / QR / remote flasher

このシーズンは「外部連携」と「機能追加」が混在。MCP Channel での Claude
からの遠隔操作、LT タイムキーパー、カメラ QR、VPN 越し flash 中継、と
独立した 4 つの機能を立ち上げつつ、内部 SRAM の慢性的枯渇と何度か向き合った。

### MCP (Claude Code Channel) adapter — Phase 1 〜 3 + 安定化

コミット: `ec0b7d7` (Phase 1), `e854c8c` (Phase 2 SSE), `cee6a68` (Phase 3 token),
`c849391` (Phase 2 hotfix), `fc23e8d` (WDT 対策)

- **Phase 1**: firmware に `/mcp/say`, `/mcp/expression`, `/mcp/balloon`,
  `/mcp/state` REST API (Bearer auth、`CONFIG_MCP_API_TOKEN`)。
  `tools/stackchan-channel/` に Bun stdio MCP adapter (4 tool 公開)。
- **Phase 2 (push events)**: firmware 側 `GET /mcp/events` を SSE
  (`text/event-stream`) で実装。adapter 側で long-poll してイベント
  (`boot` / `touch` / `say_done` / `conversation_state`) を MCP
  `notifications/claude/channel/event` に転送。
- **Phase 2 hotfix**: 当初 `/mcp/events` ハンドラがシリアル設計だったため
  esp_http_server の worker thread が長期間ブロック → `/mcp/say` が
  ~50s タイムアウト。`httpd_req_async_handler_begin` でリクエストを
  別タスクへ移譲、httpd worker を解放。`mcp_say` worker stack の
  `xTaskCreatePinnedToCoreWithCaps + MALLOC_CAP_SPIRAM` 化。
- **Phase 3 (NVS-backed token)**: `DeviceConfig::mcp_api_token` + NVS
  key `mcp_token`、BLE chr `0x1d` (write-only)、HTTP `POST /api/mcp-token`、
  Web UI 両方 (settings.html / settings_wifi.html) にランダム生成/表示/
  クリア ボタン。Kconfig 値はフォールバックとして残す。
- **WDT 対策**: CPU 1 飽和で IDLE1 が 5 秒以上動けず task_wdt 発火 →
  画面 touch も取りこぼし。`mcp_say` worker を CPU 0 へ移動、LED gradient
  を 30Hz → 10Hz に。これで実用負荷でも task_wdt は出なくなった。
- **MCP tunnels (Anthropic 公式)**: 調査の結果 Claude Code は非対応
  (Managed Agents / Messages API のみ)。現状の Cloudflare Tunnel + 直
  HTTP URL が Claude Code 向けには唯一の現実的選択肢。外部アクセスの
  最終決定 (Cloudflare Tunnel か Tailscale か) は別途。

### 内部 SRAM 削減 — 計装と sdkconfig 段階的削減

コミット: `e1eb28f`

- 過去 2 回「推測スタック縮小」が heap layout shift で boot を壊した
  ので、まず計装を入れて実測ベースに切り替えた。
- 新規 `main/diag.{hpp,cpp}` — `heap_caps_register_failed_alloc_callback`
  で全 alloc 失敗を log (size + caps + 呼出元 + 残量)、`uxTaskGetSystem
  State` で全タスク stack HWM を 60 秒ごと dump、`MALLOC_CAP_DMA largest`
  (esp-aes が実際に依存する pool) を heap log に追加。
- `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` を defaults に。
- `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM` 16 → 10 (1 個 ~1.6 KB の
  DMA-capable 内部 RAM、Gemini Live の in-flight TX < 6 frames なので
  10 で十分): +9.6 KB
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 12288 → 8192 (HWM 実測 3.9 KB): +4 KB
- 実測効果: boot 時 INT free 59.2 → 73.7 KB、steady free 24.4 → 33.7 KB、
  **INT min (handshake 谷) 0.5 → 8.7 KB**。
- 残: NimBLE msys / MBEDTLS_SSL_IN_CONTENT_LEN は Phase B 候補として保留。
- 一度 plan α (mcp_say singleton + 小タスク縮小) を試したが、boot 時の
  largest=29 KB の取り合いで seg_buf_ alloc を壊し全 revert。**「boot
  時の largest 29 KB は conv-task の 3×8 KB seg_buf_ + WS task に既に
  予約されている」が学び**。後の QR 実装でも同じ罠を踏み、最終的に
  conv-task seg_buf を app_main で boot 直後に予約 (詳細は QR 節)。

### NVS 永続 servo enable トグル

コミット: `d08b6c5`

- `DeviceConfig::servo_enabled` (NVS key `srv_en`、default true) + BLE
  chr `0x1c` + HTTP `/api/servo-enabled` + 本体タッチ UI Settings タブ
  に「サーボ (恒久)」行を追加 (kSettingsRowH=36 で 5 行収容)。
- `kServoDisabledForDebug` constexpr を撤去、`cfg.servo_enabled` で
  起動時 VM 電源 + servo_task spawn をゲート。
- `SharedState::servo_enabled` (操作画面のランタイム脱力/復帰) とは別物
  と明記。

### LED デフォルト点灯を gradient (rainbow) に

コミット: `dcb3ef4`

- LED ドライバ動作確認が取れたので `shared_state.hpp` のデフォルトを
  `kModeGradient` (6 秒で 1 周) に。I2C 書込みパスが継続的に exercise
  されるので lgfx mutex race の soak test を兼ねる。

### LED 動作復活 (RGB565 LE + refresh_leds の RMW 排除)

コミット: `12eb3be`

- `LedStrip` のバッファ形式を 3-byte RGB → 2-byte RGB565 LE に修正
  (PY32 firmware が実際に期待する形式、docs/py32_ioexpander.md §6)。
- `Py32Expander::refresh_leds()` を read-modify-write から
  `last_count_ | bit6` の単一 write に。1 フレームの I2C トランザクション
  3 → 2 で lgfx i2c mutex race (xTaskPriorityDisinherit) の発生率を抑制。
- gemini-live セッション継続下で 1 時間連続稼働、リブート 0 を確認。

### AXP2101 boot 破損調査ツール

コミット: `07dc1c5`

- `main/i2c_dump.{hpp,cpp}` — AXP2101 / AW9523 / PY32 のレジスタを xxd
  形式で dump。HW 焼込み定数 (AXP2101 OTP=0x4A、AW9523 ID=0x23、PY32
  FW=0x41) を sanity beacon として MATCH/MISMATCH 表示。2 連読みで `*`
  揺らぎマーカー。
- `tools/i2c_decode.py` — Saleae Logic 2 の I2C CSV を「論理レジスタ
  アクセス 1 行」に畳み込む Python ツール。
- 既知 issue ([docs/known_issues.md §2](docs/known_issues.md)): M5 base
  接続時に AXP2101 内部レジスタが破損して LCD バックライト消灯。
  異常 boot キャプチャ待ち。

### LT (ライトニング トーク) タイムキーパー + Module Audio (ES8388) 対応

コミット: `7191806`

- **タイマー コア** (`main/lt_timer.{hpp,cpp}`): demo_loop が毎周期
  tick。UI から `lt_command` (1=start, 2=stop) を exchange 消費。状態は
  `lt_active` / `lt_remaining_s` (負値 = 超過カウントアップ)。通知は
  `Speech::say()` (任意かな対応に新設) で発話 → 口パク追従、吹き出し
  4 秒。設定 JSON: `{"total_s","warn_s","repeat_s","warn":{text,reading},
  "over":{text,reading}}`、`repeat_s=0` で超過通知 1 回のみ。
- **本体 UI**: 6 番目のタブ「LT」。大型 mm:ss 表示 (残り 1 分で黄色、
  超過で赤 + マイナス)、3/5/10 分プリセット (実行中ロック)、スタート/
  ストップ ボタン。
- **設定 plumbing** (face_config と同じ live-apply パターン):
  `DeviceConfig::lt_config_json` + NVS key `lt_cfg`、BLE chr `0x1e`
  (暗号化 READ/WRITE)、HTTP `POST /api/lt-config`、`SharedState::set_lt_
  config` + version カウンタを demo_loop が poll してホスト タスクの
  スタックで JSON parse しない構造。settings.html / settings_wifi.html
  両方に設定フォーム。
- **Module Audio (M144 / ES8388)** 同梱: `components/board/audio_module_
  es8388.{hpp,cpp}`。boot 時に I2C 0x10 を probe (バス スキャンはしない)、
  検出時のみ ES8388 を I2S slave 16-bit DAC playback に init し、
  M5.Speaker config に `pin_mck=GPIO0` (Config B ジャンパ位置) を追加。
  AW9523 への盲書込みはしない (AXP2101 事故の教訓)。レジスタ列は
  M5Unified の Tab5 ES8388 実装から移植。**モジュール未到着、検証保留**。
- 実機確認: 残り通知 / 超過通知 / 30 秒繰り返し / プリセット / HTTP
  live-apply (total_s=60 がタブ表示に即反映) を OK。

### QR スキャナ (camera + quirc) — Phase 1 + 2 + メモリ構造修正

コミット: `7c97d86`

- `components/board/camera_gc0308.{hpp,cpp}`: M5CoreS3 上流の GC0308.cpp
  と完全一致の camera_config_t (pin_xclk=-1 内蔵 OSC、SCCB 12/11、データ
  39/40/41/42/15/16/48/47、VSYNC=46 HREF=38 PCLK=45)、QVGA grayscale、
  fb_location=PSRAM。`begin()` は largest >= 12 KiB のフロアチェック →
  `m5::In_I2C.release()` (lgfx I2C と SCCB が GPIO12/11 を共有) →
  `esp_camera_init`。**AW9523 への盲書込みは行わない**。
- `main/qr_task.{hpp,cpp}`: `start_qr_scan(Board&)` / `stop_qr_scan()`。
  worker は **5 KiB 内部 RAM stack** (8 KiB だと largest=7424 で alloc 失敗
  したので絞った。quirc 1.2 の flood_fill は explicit-stack で C-stack 再帰
  なしと検証済)、CPU 0 prio idle+2、3 秒の同一ペイロード抑制。
- `main/Kconfig.projbuild`: `STACKCHAN_QR_TEST_AT_BOOT` (default n) で
  boot+30s に scan を 1 度起動するテスト パス。P3 で device_ui に移植予定。
- **重大なメモリ再構成**: conversation_task の 3×8 KiB seg_buf_ を
  「Wi-Fi up 時点 (largest=10 KiB)」に確保していたのを `app_main` が
  Board::begin() 直後 (largest=29 KiB) に予約 → `ConversationTaskArgs::
  seg_buf` で渡す方式へ。これで esp32-camera リンクで .dram0.data が
  増えても conv-task が壊れない。失敗時は nullptr で conversation のみ
  自動 disable。
- **esp32-camera センサー絞り込み**: GC0308 以外 14 種 (OV7670/OV2640/
  HM1055 等) のドライバを Kconfig で無効化、.dram0.data -8.8 KiB。
- **未検証**: 実機での QR 提示 → `qr: decoded:` ログ確認は会話 OFF に
  してから別途。前回の試行で「Wi-Fi bring-up のピークで内部 RAM が
  枯渇 → coex の esp_timer 操作中に timer リスト破損で panic」という
  連鎖を 2 回経験しているので、最終的な定常動作確認はまだ。
- Phase 3 (device_ui タブ) / Phase 4 (BLE 通知) は P1+P2 検証後に着手。

### Remote flasher — VPN 越しブラウザ経由で ESP を flash する中継

コミット: `91acefc`

- **背景**: 開発機が VPN の向こうにあり、ESP はホスト LAN にぶら下がっている
  状況で「リモートの Claude Code / CI から idf.py flash したい」を解決。
- **ホスト側 Bun サーバー** (`tools/remote-flasher/server/server.ts`):
  単一ファイル、外部 npm 依存ゼロ。`POST /flash` (multipart、`meta` JSON
  + 各セクションのファイル パート) を受けて SSE で progress / done を流す。
  `GET /ws` でブラウザ クライアント 1 接続のみ (排他)。`POST /reset`、
  15 秒 ping、5 分タイムアウト、ブラウザ未接続 → 503、flash 中 → 409。
- **ブラウザ側 Web UI** (`tools/remote-flasher/web/`): index.html
  単一ページ + esptool-js v0.5.7 を bun build で IIFE bundle (~182 KB)。
  WebSerial で実機 port を選択 → ESPLoader.main() でチップ自動判別 →
  WS 接続 → flash_request 受信 → ESPLoader.writeFlash → progress/done
  を WS で返信。ダーク テーマ。WebSerial 非対応ブラウザは検出して無効化。
- **プロトコル文書** (`tools/remote-flasher/PROTOCOL.md`): host→browser
  は flash_request JSON 直後にバイナリ フレーム N 個を連送 (1 セクション
  = 1 frame、宣言順)。browser→host は hello / progress / done / log /
  pong。ライフサイクル図 + エラー処理 + SSE と HTTP ステータスの境界。
- **統合テスト** (本セッション、WS スタブ): サーバー起動 → no-browser
  /flash で 503 → WS スタブ接続 → multipart /flash (2 セクション 1KB+
  2KB) → flash_request JSON + バイナリ フレーム 2 個 (順序・サイズ一致)
  → スタブが progress×2 + done を返信 → /flash の SSE レスポンスに
  正しく転送 → /reset → WS reset フレーム送信。**全パス**。
- **実機確認は未** (本物の WebSerial + ESP32 chip)。次は curl ラッパ
  スクリプト (`build-cores3/` から bootloader/partition/app を集めて
  POST する `flash-current-build.sh` 的なもの) を追加する。

### 持ち越し課題 (このセッション分)

- **QR 実機テスト**: 会話 OFF にしてから boot+30s スキャン → QR 提示
  → `qr: decoded:` 確認。前回 8 KiB stack で alloc 失敗 → 5 KiB に絞った
  版を flash 済み (未テスト)。
- **Module Audio (ES8388)** 実機検証: ハードウェア到着待ち。
- **AXP2101 broken-boot キャプチャ**: 再発時に Saleae で取得 →
  `tools/i2c_decode.py` で解析、の予定。
- **SRAM Phase B** (NimBLE msys / MBEDTLS_SSL_IN_CONTENT_LEN): conv-task
  再接続で esp-aes 失敗が再発したら手をつける。`min=8679` まで回復した
  ので長時間運用しないと再発しないはず。
- **Remote flasher curl ラッパ**: `tools/remote-flasher/flash-current-
  build.sh` で `make build` 後のバイナリを POST するスクリプト。**次の
  作業対象**。
- **QR Phase 3 (device_ui タブ) + Phase 4 (BLE 通知)**: P1+P2 実機検証後。
- **AtomS3R Phase 2**: スコープ未定。
- **外部アクセス手段選定**: Cloudflare Tunnel か Tailscale か。

### 今シーズンで学んだこと (次回への申し送り)

1. **boot 時 largest=29 KiB の取り合い**: conv-task の 3×8 KiB seg_buf_
   と WS task の 8 KiB が「Wi-Fi up 時点 (largest=10 KiB)」で確保されて
   いるため、それまでに新しい内部 RAM 消費者を追加すると壊れる。新機能
   の永続バッファ・タスク スタックは `app_main` 序盤に予約するか、
   PSRAM に逃がすこと。
2. **esp32-camera は managed component の Kconfig が罠**: 14 種の使わ
   ないセンサー ドライバが .dram0.data に +8.8 KiB 乗ってくる。新しい
   managed component を導入したら map ファイルで .dram0.data の増分を
   確認 + 不要オプションを切る。
3. **PSRAM スタック禁止ルール** (cache 汚染 + Speaker/Mic 6 との相性):
   `mcp_say` は唯一の例外 (singleton 8 KiB internal を試したら seg_buf_
   を奪って boot 壊した経緯あり)。
4. **quirc 1.2 の flood_fill は explicit-stack** — タスク スタックを
   ケチっても問題ない (5 KiB で動く)。
5. **httpd 設計**: esp_http_server の worker は単一スレッドなので長期
   接続 (SSE) は `httpd_req_async_handler_begin` で別タスクに移譲必須。
   そうしないと他のリクエストが全部詰まる。
6. **計装は先に入れる**: スタック サイズ縮小は HWM (`uxTaskGetSystemState`)
   実測なし → 危険。alloc 失敗は `heap_caps_register_failed_alloc_
   callback` で size + caps + caller を取れる。両方常時入れておくべき。

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
