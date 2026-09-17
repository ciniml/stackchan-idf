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

## 3. USB-JTAG 接続中のパニックで再起動せず「ゾンビ」化 (対策済み)

**症状**: アイドル発話・顔アニメ・サーボが止まるが HTTP は応答する。
吹き出しが出ず、`heap:` ログも止まる。電源再投入で復帰。

**発生履歴**: 2026-09-16〜17 CoreS3。USB-C を PC に繋いだまま運用中に 3 回。
繋いでいないときは代わりに再起動 (TG1WDT_SYS_RST) として現れていた。

**確認した状態 (OpenOCD で無停止アタッチ)**: CPU1 が ROM の起動待ちループ、
CPU0 は正常、TIMG0/TIMG1/RTC の全 WDT が無効、`pxCurrentTCBs[1]` = render、
`balloon_in_flight` = true (吹き出し完了コールバックは render 側なので永久に来ない)。

**原因**: `CONFIG_ESP_DEBUG_OCDAWARE=y` (IDF 既定)。パニックハンドラは
`esp_cpu_dbgr_is_attached()` が真だと WDT を全部止め、他コアを stall し、
ブレークポイントを仕掛けて**復帰する** (再起動しない)。USB-JTAG が PC に
列挙されているだけでこの経路に入る。元のパニックは状況証拠から CPU1 の
割り込み WDT (render 実行中)。

**対策**: `CONFIG_ESP_DEBUG_OCDAWARE=n` で常に再起動させる。あわせて
espcoredump をフラッシュ (ADR-001 exttab の `coredump` 64 KiB) に保存し、
起動時ログと `GET /api/coredump` で前回パニックの要約 (タスク名 / PC /
原因 / バックトレース) を取れるようにした。**元のパニックの根本原因は未解決**
— 次回発生時に `/api/coredump` の PC / バックトレースを addr2line で解析する。

## 4. v0.13.0〜v0.14.0 のリリース版 Recovery は OTA 取得ができない (対策済み)

**症状**: BLE / Wi-Fi どちらの設定画面からリリースを選んでも、Main が Recovery へ
再起動した後に `release-fetch failed before receiving — returning to main` で
元のファームウェアに戻る。

**原因**: GitHub Pages のカスタムドメインは `https://ciniml.github.io/...` への
要求を **`http://`**www.fugafuga.org への 301 で返す。ESP-IDF v5.5.5 以降の
`esp_http_client_set_redirection()` はこの https→http リダイレクトを
`ESP_ERR_HTTP_REDIRECT_DOWNGRADE` で拒否する (CI は `release-v5.5` ブランチ先端で
ビルドしていたため該当、手元の 5.5.4 では再現しなかった)。

**対策**: `https_fetch.cpp` で Location ヘッダを HTTP イベントで捕捉し、自前で
https へ昇格して `esp_http_client_set_url()` で再接続する (set_redirection 不使用)。
ローカル / CI とも IDF を **v5.5.5 タグに固定**。

**注意**: Recovery は OTA で更新されないため、上記バージョンのリリース版を
Web flasher で入れた機体は、修正版を **もう一度 USB (Web flasher) で書き込む**
必要がある。それ以降は OTA が使える。

## 5. AtomNyan のネコミミ LED が v0.12.0〜v0.14.1 で壊れる (対策済み)

**症状**: 1 個目の LED だけ黄色で高輝度に光り、残りが消灯。`led` タスクは生きていて
`led_strip_refresh` も成功する。v0.11.0 までは正常。

**原因**: ネコミミの WS2812 データ線 GPIO 38 は Atomic ECHO BASE の I2C SDA と
共用。M5Unified はコーデック (ES8311) を叩くたびに M5GFX の
`i2c_temporary_switcher_t` で I2C1 を GPIO 38/39 に一時的に載せ替え、終わったら
`pin_backup_t` でピン設定を元に戻す。この復元が動かなくなった直接の原因は
**リポジトリ内に M5GFX が 2 コピーあり、ヘッダとコードで版が食い違う ABI 不整合**:
- M5Unified / board / main は include 順で先に来る submodule `components/M5GFX`
  (0.2.23) の**ヘッダ**でコンパイルされる。
- リンクされる**コード**はほぼ全て managed component `m5stack/m5gfx` (M5Unified の
  idf_component.yml が要求。dependencies.lock が gitignore だったので CI は毎回
  最新: v0.12.0 で 0.2.28、以降 0.2.29) 側。
- 0.2.27 以降は `pin_backup_t` に `_gpio_out` が増えて構造体サイズが変わり、0.2.23
  レイアウトで確保した `i2c_temporary_switcher_t` を 0.2.28 のコードが操作する。
  実機ログでは restore 時に `_backuped=76 _need_reinit=40` というゴミ値が出て復元が
  スキップされ、GPIO 38 が I2C (オープンドレインの素の GPIO) のまま残る。
- 0.2.24〜0.2.26 はレイアウト互換だったため偶然無事。手元は lock が 0.2.23 だった。
M5GFX 上流のバグではない (上流への報告は不要)。

**確認方法**: IDF は関係ない (v0.12.0 のソースを IDF v5.5.5 でビルドしても再現)。
現行ソースを m5gfx 0.2.27〜0.2.29 でビルドすると `nekomimi-led: GPIO 38 was
re-configured (sig lost, pad open-drain)` が出る (0.2.24〜0.2.26 では出ない)。
OpenOCD 経由の GPIO レジスタ読み書きは CPU 側の実態と一致しないので、この
切り分けには使えない。

**対策 (v0.14.2)**:
- `NekomimiLedStrip::show()` の先頭で GPIO 38 の出力信号 (RMT) とパッド設定を
  毎フレーム確認し、崩れていれば復元する (`restore_pin_routing`)。
- `main/idf_component.yml` で `m5stack/m5gfx` を submodule と同じ `==0.2.23` に固定
  (ヘッダとコードの版を一致させる対症療法)。
- `dependencies.lock` をコミット対象にし、CI とローカルで managed component の
  解決結果を一致させる (`.gitignore` から除外)。

**根本対策 (実施済み, 2026-09-18)**: M5GFX を 1 コピーにした。submodule を
`third_party/m5gfx` (components/ の外 = 自動検出されない) へ移し、
`main/idf_component.yml` の `m5stack/m5gfx` を `override_path: ../third_party/m5gfx`
でそこへ向ける。M5Unified (`m5gfx` を REQUIRES) も board / avatar / avatar_vm / main
も同じソースのヘッダ・コードになり、リンクマップにも `libm5gfx.a` しか現れない。
版更新は submodule の更新だけで済む。component manager は lock のローカル
パスを絶対パスで書き戻すので、`make build` / `make set-target` の後に
`make normalize-lock` で `third_party/m5gfx` の相対形に戻している (相対形のまま
構成が通ることは確認済み)。

## 6. (将来用) ここに追記してください

