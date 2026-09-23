<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# CoreS3 内蔵 近接センサー (LTR-553ALS) で反応する機能 — 調査と設計案

2026-09-24。状態: **実装済み** (§6 に実装メモ)。§3 の案は決定事項として実装した。

## 1. ハードウェア

| 項目 | 内容 |
|---|---|
| センサー | LiteOn **LTR-553ALS-WA** (環境光 ALS + 近接 PS、赤外 LED 内蔵)。CoreS3 前面、LCD の上辺付近 |
| バス | 内部 I2C、アドレス **0x23** (PY32 0x6F / Si12T タッチ 0x68 / AXP2101 0x34 と同じバス)。`m5::In_I2C` で読む |
| M5Unified | README の機器表に載っているだけで**ドライバ無し** → 自前ドライバ (`si12t_touch.cpp` と同じ作り) |
| 識別 | PART_ID `0x86` = 0x92、MANUFAC_ID `0x87` = 0x05 |
| 近接の主要レジスタ | PS_CONTR `0x81` (bit1:0 = 11 で active)、PS_LED `0x82` (周波数 / duty / 電流 5〜100 mA)、PS_N_PULSES `0x83`、PS_MEAS_RATE `0x84` (50 ms〜2 s)、PS_DATA `0x8D-0x8E` (11 bit、0〜2047、bit7 of 0x8E = 飽和)、ALS_PS_STATUS `0x8C`、PS しきい値 `0x90-0x93` + INTERRUPT `0x8F` |
| ALS (おまけ) | ALS_CONTR `0x80`、ALS_DATA `0x88-0x8B` (2 ch)。今回は使わないが同じドライバで読める (将来: 暗いと画面を暗くする等) |

値の性質: PS_DATA は**反射光量**なので距離に単調ではあるが非線形 (数 cm で急増)、手の色・環境の赤外 (直射日光) で変わる。**閾値は固定値を決め打ちできず、実機で測って既定値を置き、利用者が調整できる必要がある** (これが設定項目を設ける理由)。割り込みピンは CoreS3 では使わない (ポーリングで十分: 50〜100 ms)。

## 2. 既存の仕組みとの関係 (調査結果)

- 「なでなで」は別センサー (Si12T 静電容量 @0x68、M5 ベース側)。近接は**本体側**なので Takao ベースでも使える
- ポーリングは `main/demo_loop.cpp` の 50 ms ループ (app_main タスク) に集約されている: `M5.update()` / 電池 (5 s) / IMU 振動 / なでなで。**カメラ使用中は `SharedState::i2c_quiesce` で In_I2C ポーラーが止まる規約**があり、近接もここに乗せる (別タスクにすると In_I2C の無ミューテックス競合を増やす — JOURNAL.md:801)
- なでなでの反応 (demo_loop.cpp:497-545): `speech.stop()` → 表情 Happy → 吹き出し「なでなで♡」2.2 s → 首を ±8° 振る → クールダウン 4 s。MCP イベント `publish_touch_stroke` も出す
- ボード判定: `Board::begin()` の CoreS3 経路で **probe 成功 → ポインタ、失敗 → nullptr** (タッチと同じ)。`BoardProfile` に `has_proximity` を足して UI/BLE に伝える
- 設定の配管 (`barge-in` が最良のテンプレ):
  1. `config_service.hpp` にフィールド
  2. `settings_registry.cpp` の `kTable` に行 (`num_row` / `bool_row`、`ApplyKind::Immediate`、nvs_key は追記のみ)、`kSettingCount` を +N
  3. `main/settings_sinks.cpp::on_config_change` に `else if (id == ...)` を 1 つ → `SharedState` の atomic へ
  4. BLE: `gatt_settings.cpp` に UUID (**次の空きは 0x30**)、handle、read/write 分岐、サービス表の行 (値は AES-GCM、生バイト)
  5. HTTP: `handle_*_post` + `add(server, "/api/...")`。`GET/POST /api/settings` と `/api/reboot-required` は表駆動で自動
  6. ページ: `tools/settings.html` (BLE、boardKind で表示制御) と `settings_wifi.html` (`BADGE_FIELDS` に id を足す)、`tools/ble-cli` は汎用 read/write で対応不要
- **注意点 (要決定)**: `ApplyKind::Immediate` は HTTP では即時反映だが、**BLE では現状 staging → Apply (再起動) が必要** (`http_handlers.cpp:64-69` の Phase 1 メモ)。閾値をいじって手をかざして確かめる用途では再起動は致命的なので、BLE 側も Immediate 行は即時反映にする (汎用の変更、他の Immediate 設定 = barge-in / speaker-volume も恩恵) のを推奨

## 3. 機能の設計案

### 3.1 「近接時の反応」の中身 (案)

近接 (near) を**イベント**として検出し、反応は「なでなで」と同じ部品で組む:

| 段階 | 反応 (既定) | 備考 |
|---|---|---|
| near 立ち上がり (手が近づいた) | 表情 → Happy、視線を正面へ、吹き出し「なに？」+ 近接用フレーズ (jtts) を 1 つ発話 | フレーズは `jtts_cfg` の既存フレーズ表に `"proximity"` 群を追加 (設定ページで編集可)。フレーズ空なら吹き出しだけ |
| near 継続中 | 表情維持、視線が手の方向 (左右は取れない: 1 ch なので正面固定)、random pose を止める | 「見つめる」だけの安価な反応 |
| far 立ち下がり | 表情を元に戻す | 発話はしない |
| 会話中 | **反応しない** (idle 時のみ: `allow_full_demo` / `conv.idle` ゲート) | なでなでは会話中も動くが、近接は誤発火が多いので idle 限定が安全 |
| MCP | `proximity.near` / `proximity.far` イベントを SSE で通知 (raw 値付き) | 外部連携 (`publish_touch_stroke` と同型) |

将来の拡張 (今回はやらない): 手を振る (near/far の繰り返し) ジェスチャ、ALS で暗所時の画面減光、ESP-NOW リモコン運転中の扱い。

### 3.2 検出ロジック (demo_loop の 50 ms ループ)

```
raw = PS_DATA (11 bit)          … 100 ms ごとに読む (PS_MEAS_RATE=100 ms に合わせる)
near 判定: raw >= near_threshold が hold_ms 連続      → near (ヒステリシス上側)
far  判定: raw <  far_threshold  が hold_ms 連続      → far  (下側、near_threshold より小さい)
反応の再発火: 前回の near 反応から cooldown_ms 以上
```
飽和ビット (0x8E bit7) が立っていれば near 扱い。カメラの quiesce 中は判定を凍結 (状態は保持)。

### 3.3 調整項目 (設定、すべて `ApplyKind::Immediate`)

| 設定 id | nvs_key | 型 / 範囲 | 既定 (仮 — 実機で決める) | 意味 |
|---|---|---|---|---|
| `prox-enabled` | `prox_en` | bool | 1 | 近接反応の有効 / 無効 |
| `prox-near` | `prox_near` | U16 0..2047 | 300 | near と判定する PS 値 (以上) |
| `prox-far` | `prox_far` | U16 0..2047 | 150 | far に戻す PS 値 (未満)。near より小さく (検証で強制) |
| `prox-hold-ms` | `prox_hold` | U16 0..2000 | 200 | 判定に必要な連続時間 (誤発火抑制) |
| `prox-cooldown-s` | `prox_cool` | U8 0..120 | 10 | 反応の再発火間隔 |

LED 電流 / パルス数 / 測定周期は**固定** (既定 50 mA / 1 pulse / 100 ms。反射量が変わるので閾値と連動してしまい、利用者に触らせない)。

### 3.4 調整のための可視化 (重要)

閾値は数字だけでは決められないので、**現在の生値をページに出す**:
- HTTP: `/api/status` に `"proximity": {"raw": N, "near": bool, "available": bool}` を追加 (ページは既に status をポーリング)
- BLE: 読み取り専用 chr 1 つ (UUID 0x31、`u16 raw + u8 near`)。設定ページの近接セクションに「現在値 ▮▮▮▯▯ 412 (near)」のバーと、**「今の値を near 閾値にする」** ボタン (far は near × 0.5 を自動提案)
- MCP イベントでも raw を含める

### 3.5 ボード対応

- `BoardProfile::has_proximity` = CoreS3 (M5Base / TakaoBase) のみ true。実際の有効化は **probe 成功** (PART_ID 一致) で決める (タッチと同じ)。AtomS3R / StopWatch には無い → 設定セクションは `boardKind` で隠す
- `/api/status` の `proximity.available` と BLE の BoardKind で UI を出し分け

## 4. 実装ステップ

1. **ドライバ + 生値の計測 (bring-up)**: `components/board/ltr553_proximity.{hpp,cpp}` (probe / configure / read_ps / read_als)、`Board::proximity_sensor()`、`i2c_dump` の既知チップ表に 0x23 追加。demo_loop で 100 ms ごとに raw をログ (一時的に INFO) → 実機で「手なし / 20 cm / 10 cm / 5 cm / 密着」「直射日光」「暗所」を測り、既定閾値を決める
2. **検出 + 反応**: 3.2 / 3.1 を demo_loop に実装 (なでなでブロックと同じ場所。`docs/refactoring-survey.md` が指摘する demo_loop 肥大は、近接とタッチを `main/input_events.cpp` に切り出して緩和)。MCP イベント
3. **設定の配管**: 3.3 の 5 行 (registry / sinks / BLE 0x30 / HTTP / 2 ページ / BADGE_FIELDS)。**BLE の Immediate 即時反映**を同時に入れる
4. **可視化**: 3.4 (status / BLE 0x31 / ページのバーと「今の値を閾値に」)
5. 近接フレーズ (`jtts_cfg` に `proximity` 群)、ドキュメント (`docs/ltr553_proximity.md` にレジスタと調整手順)

工数感: 1 は半日 (実機計測込み)、2〜4 で 1〜1.5 日、5 で半日。

## 5. 決めてほしい点

1. **反応の内容** (3.1 の既定で良いか): Happy + 吹き出し + 近接フレーズ発話 + far で戻す。首振りは付けない (なでなでとの区別)
2. **会話中は反応しない** で良いか
3. **BLE でも Immediate 設定を即時反映**にする (汎用変更) で良いか — 推奨
4. **近接フレーズ**を jtts フレーズ表に足す (設定ページで編集可) で良いか、それとも吹き出しのみにするか

## 6. 実装メモ (2026-09-24)

- 実機計測 (LED 100 mA / 8 パルス / ゲイン x16): 手なし 19〜28、20 cm 28〜32 (床と区別不能)、10 cm 38〜46、5 cm 75〜90、密着 1500〜2047。
  50 mA / 1 パルスでは密着でしか反応しなかった (~150)。既定閾値 near=60 / far=35 / hold=200 ms / cooldown=10 s
- ドライバ `components/board/ltr553_proximity.{hpp,cpp}`: `PsConfig` (ゲイン x16/x32/x64、LED 周波数 / duty / 電流、パルス数、測定周期、PS_OFFSET) を `configure()` で
  standby → 書き込み → active。PART_ID 0x92 / MANUFAC_ID 0x05 で probe
- 検出 + 反応は `main/demo_loop.cpp` (100 ms サンプル、ヒステリシス + hold、near で Happy + 近接フレーズ (`jtts_cfg.proximity_phrases`、無ければ吹き出し「なに？」)、far で表情復帰、会話中は反応しない、MCP `proximity` イベント)。
  前段の再設定は `SharedState::proximity.sensor_dirty` を見て demo_loop が行う (In_I2C を触るのは demo_loop だけ)
- 設定 12 行 (`prox-enabled/near/far/hold-ms/cooldown-s` + 調整モード `prox-gain/led-freq/led-duty/led-current/pulses/meas-rate/offset`)、全部 `ApplyKind::Immediate`
- BLE: chr **0x30** (閾値 + 現在値、12 B 読 / 8 B 書)、chr **0x31** (調整モード、11 B 読 / 8 B 書)。**どちらも BLE でも即時反映** (staging を通さない)
- HTTP: `GET/POST /api/proximity` (JSON、部分更新可)、`/api/status` に `proximity_*`
- ページ: `tools/settings_common.js` の共通セクション (現在値バー + 「今の値を near にする」+ 調整モード `<details>`) を両ページに挿入。CoreS3 以外では非表示
- 確認: ble-cli で 0x30 / 0x31 の往復、ゲイン x64 書き込みで即時に `PS active … gain x64` と near 発火 (床値が 4 倍になるため) → x16 に戻すと far
