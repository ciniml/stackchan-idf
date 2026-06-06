<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# Known issues

stackchan-idf の調査中 / 未解決 / 既知バグの一覧。各エントリは
**症状 → 再現条件 → 推定原因 → 暫定対応 → 根本対策候補** の順。

---

## 1. lgfx i2c mutex race による定期再起動 (long-run reboot)

**症状**: 数十分連続動作させていると `xTaskPriorityDisinherit` の assert で
リブート ループに陥る。

**発生履歴**:
- 2026-06: M5 base CoreS3 で AXP2101 / LED 調査中に頻発。
  led_task (30Hz @ Core 1) + conversation_task (Gemini Live @ Core 0) を
  同時運用しているとき。

**再現条件**:
1. M5 base CoreS3 (M5 Stack-chan base、PY32 LED strip 有り) で起動
2. Gemini Live / OpenAI Realtime backend で会話を始める (= 連続 I2S 駆動)
3. led_task を活発化させる (mode=Solid / Breath / Rainbow 等)
4. **10〜40 分**程度連続動作 → 突然 panic

**スタック トレース** (一例、`gemini-live: audio send seq=51701` 時点):
```
assert failed: xTaskPriorityDisinherit tasks.c:5147
              (pxTCB == pxCurrentTCBs[ xPortGetCoreID() ])

Backtrace:
  panic_abort                  esp_system/panic.c:469
  esp_system_abort             esp_system/port/esp_system_chip.c:87
  __assert_func                newlib/assert.c:80
  xTaskPriorityDisinherit      FreeRTOS-Kernel/tasks.c:5147
  prvCopyDataToQueue           FreeRTOS-Kernel/queue.c:2469
  xQueueGenericSend            FreeRTOS-Kernel/queue.c:964
  lgfx::v1::i2c::i2c_context_t::unlock()
                               M5GFX/.../common.cpp:1037
  i2c_wait                     M5GFX/.../common.cpp:1311
  lgfx::v1::i2c::endTransaction()
                               M5GFX/.../common.cpp:1689
  lgfx::v1::i2c::transactionWriteRead()
                               M5GFX/.../common.cpp:1840
  lgfx::v1::i2c::readRegister8()
                               M5GFX/.../common.cpp:1846
  m5::I2C_Class::readRegister8()
                               M5Unified/.../I2C_Class.cpp:87
  stackchan::board::Py32Expander::refresh_leds()
                               components/board/io_expander_py32.cpp:148
  stackchan::board::LedStrip::show()
                               components/board/led_strip.cpp:49
  stackchan::app::(anonymous namespace)::led_task_entry()
                               main/led_task.cpp:117
```

**推定原因**: lgfx の `i2c_context_t` が `xSemaphoreTake` + `xSemaphoreGive` で
mutex を回しているが、ESP-IDF の priority-inheritance assert は **mutex を
取ったタスクと放したタスクが同一**で **しかも現在実行 中のタスク**であることを
要求する。我々の構成では:
- led_task: Core 1、priority 2、`m5::In_I2C` を 30Hz で叩く (毎フレーム 3 トランザクション = `read REG_LED_CFG` + `write count` + `write refresh-bit`)
- conversation_task: Core 0、`m5::In_I2C` 直接は触らないが、近傍で I2S 駆動 + WebSocket 受信 (CPU 高負荷 + 割込)
- 加えて render_task / servo_task / battery monitor 等が間欠的に `In_I2C` を触る

長時間運用中 にスケジューリングのコーナー ケースで mutex 取得/解放 ペアが
タスク跨ぎになり、assert で落ちる。lgfx upstream の I2C 同期実装の限界と考える。

**暫定対応** (実装済): (D) 案 — `Py32Expander::refresh_leds()` の RMW 排除
- `Py32Expander` に `last_count_` メンバを持ち、`set_led_count()` で最後に書いた
  count をキャッシュ。`refresh_leds()` は `last_count_ | bit6` の単一 write に
  なり、1 フレーム あたり I2C 3 transaction → **2 transaction** に削減
  (RAM burst + CFG write の 2 本)
- 実装: [components/board/io_expander_py32.cpp](../components/board/io_expander_py32.cpp)
  の `refresh_leds()` および
  [components/board/include/board/io_expander_py32.hpp](../components/board/include/board/io_expander_py32.hpp)
  の `last_count_` メンバ

**観測結果** (2026-06-06):
- gemini-live セッション継続 (= Core 0 で I2S + WebSocket フル負荷) +
  led_task 30Hz @ Core 1 を **1 時間** 連続稼働 → panic / assert / リブート ゼロ
- 過去は同条件で 10〜40 分で `xTaskPriorityDisinherit` を引いていた
- 1h クリアは強い好転シグナルだが race は確率的なので、長時間 (数時間 / 一晩)
  または多回試行で確証を取りたい

**残り根本対策候補** (今後、(D) で再発する場合):
- **(C)** led_task の `kPeriodTicks` を `pdMS_TO_TICKS(33)` (30Hz)
   → `pdMS_TO_TICKS(100)` (10Hz) に落とす。breathing / rainbow には十分
- **(B)** `m5::In_I2C` を直接触らず、専用 I2C ワーカ タスクに集約してキュー
   経由で叩く ↔ 大幅 refactor
- **(A)** M5GFX upstream の `i2c_context_t` 実装にバグ報告。優先継承 mutex を
   FreeRTOS-Kernel native と整合させる修正を提案 ↔ 上流に手を入れる作業量大

---

## 2. M5 base 接続時に AXP2101 内部状態 が破損 (LCD バックライト消灯)

**症状**: M5 base 取り付け CoreS3 で LED 制御コードを有効にしていると、起動
からしばらく して LCD バックライトが消灯する。電源完全断 (USB + バッテリ
同時抜き) で復旧、ソフト リセットでは復旧しない。

**発生履歴**:
- 2026-05: 初出。当時は AXP2101 への直接書込が無いコード だったが、I2C scan
  コードが入っていた → scan 除去で一度収まる
- 2026-06: PY32 LED 制御コード復活で再発、間欠的

**再現条件**: 完全には特定できていない。LED 制御コード (Py32Expander 経由の
PY32 0x6F へのアクセス) が走ると 数十分〜数時間で発生する模様。

**ダンプ証跡** (M5 base、`docs/py32_ioexpander.md` 仕様 と比較):
- AXP2101 reg `0x03` (OTP/FW version、HW 焼込み固定値) が `0x4A` のはずが
  **`0x0C`** で返る (sanity beacon MISMATCH)
- AXP2101 reg `0x20`〜`0xA3` が **全 0** (健全 Takao base では値が見える)
- AXP2101 reg `0x90` (LDO enable) = `0x00` → DLDO1 (= CoreS3 LCD バック
  ライト) disabled → 画面表示は生きてるがバックライトだけ消える
- AW9523 / PY32 は正常応答 (read は安定、ID / 既知定数 一致)
→ AXP2101 だけが「I2C ACK は返すが内部レジスタ群が壊滅状態」

**推定原因** (確証 無し):
- 仮説 A: PY32 LED 制御中の電気的ノイズ (WS2812 高周波信号、サーボ電源
  on/off の突入電流) が I2C バス や AXP2101 入力に誘導 → AXP2101 内部
  状態機 が落ちる
- 仮説 B: 何らかのコード パスが AXP2101 に意図しない書込を出している (現
  コード ベースでは grep 上見つからない)
- 仮説 C: AXP2101 自体の thermal / UV fault モードに入って rail を全部 off
  したまま再 init を受け付けなくなる

**調査中の手段** (`main/i2c_dump.cpp`):
- 起動直後に AXP2101 / AW9523 / PY32 のレジスタを一気にダンプ
- `0x03` 等の HW 既知定数で sanity beacon
- 2 連読で `*` 不安定マーカ
- AXP2101 が writes を受け付けるかの probe (`reg 0x90 = 0xBF` 強制書込 → 再読)

**現状の暫定対応**:
- 物理: 発生したら USB + バッテリ同時抜き
- ソフト: `kLedTaskDisabledForDebug = true` (項目 1 の措置と兼用) で
  繰返し I2C 駆動を抑制

**根本対策候補**:
- AXP2101 write probe の結果次第:
  - 「書込みが効く」なら起動時に M5Unified の AXP2101 init を再実行する
    回復ルーチンを追加
  - 「効かない」なら原因は電気的、対策はハードウェア側 (バイパス コンデン
    サ追加、I2C プルアップ強化、LED 電源分離) になる

---

## 3. (将来用) ここに追記してください

