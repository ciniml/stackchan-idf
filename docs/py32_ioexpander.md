<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# PY32 IO Expander — 仕様メモ

M5Stack の StackChan ベース基板に載っている **PY32 マイコン (puyu PY32F003)**
を I2C スレーブ化した IO エキスパンダー (PY32IOExpander) の仕様。GPIO 拡張
+ ADC + PWM + WS2812 NeoPixel 専用 RAM/ドライバを 1 チップに統合している。

このドキュメントの一次ソースは M5Stack 公式 BSP
[`~/repos/StackChan-BSP/src/drivers/PY32IOExpander/`](file:~/repos/StackChan-BSP/src/drivers/PY32IOExpander/PY32IOExpander.cpp)
(M5Stack Technology / MIT License)。**本リポジトリの実装は読み取り専用参照**で、
コードはそのまま流用しない方針 (`CLAUDE.md`)。

---

## 1. I2C パラメータ

| 項目 | 値 |
|---|---|
| デフォルト I2C アドレス | **`0x6F`** |
| バス | CoreS3 / Takao ベースの **内部 I2C** (M5Unified の `In_I2C`) |
| クロック | 100 kHz (BSP デフォルト、400 kHz でも動作する見込み) |
| トランザクション形式 | `S + ADDR_W + REG + DATA*N + P` (典型的な 8bit レジスタ ポインタ + データ) |

> **⚠️ 重要**: 同じ内部 I2C バスに **AXP2101 (`0x34`)** が乗っている。
> AXP2101 は M5Unified の電源管理ドライバが排他管理する。**本ドライバから
> `0x34` には絶対に触らない** (誤書込みで LCD バックライト等が消える)。
> アドレス未確認の I2C scan は禁止。

---

## 2. レジスタ マップ

| アドレス | 名前 | RW | 用途 |
|---|---|---|---|
| `0x00` | `REG_UID_L` | R | デバイス UID 下位バイト |
| `0x01` | `REG_UID_H` | R | デバイス UID 上位バイト |
| `0x02` | `REG_VERSION` | R | FW バージョン (0/0xFF は未応答扱い → `begin()` で false) |
| `0x03` | `REG_GPIO_M_L` | RW | GPIO 方向 (pin 0-7): 0=入力, 1=出力 |
| `0x04` | `REG_GPIO_M_H` | RW | 同 (pin 8-15) |
| `0x05` | `REG_GPIO_O_L` | RW | GPIO 出力レベル (pin 0-7) |
| `0x06` | `REG_GPIO_O_H` | RW | 同 (pin 8-15) |
| `0x07` | `REG_GPIO_I_L` | R | GPIO 入力レベル (pin 0-7) |
| `0x08` | `REG_GPIO_I_H` | R | 同 (pin 8-15) |
| `0x09` | `REG_GPIO_PU_L` | RW | プルアップ有効 (pin 0-7) |
| `0x0A` | `REG_GPIO_PU_H` | RW | 同 (pin 8-15) |
| `0x0B` | `REG_GPIO_PD_L` | RW | プルダウン有効 (pin 0-7) |
| `0x0C` | `REG_GPIO_PD_H` | RW | 同 (pin 8-15) |
| `0x0D` | `REG_GPIO_IE_L` | RW | 割込み許可 (pin 0-7) |
| `0x0E` | `REG_GPIO_IE_H` | RW | 同 (pin 8-15) — 上位 0-5 ビットのみ有効 (pin 8-13) |
| `0x0F` | `REG_GPIO_IT_L` | RW | 割込みトリガ方式 (edge/level) |
| `0x10` | `REG_GPIO_IT_H` | RW | 同 (上位) |
| `0x11` | `REG_GPIO_IS_L` | RW | 割込みステータス (1 書込でクリア) |
| `0x12` | `REG_GPIO_IS_H` | RW | 同 (上位) |
| `0x13` | `REG_GPIO_DRV_L` | RW | ドライブ モード: 0=push-pull, 1=open-drain |
| `0x14` | `REG_GPIO_DRV_H` | RW | 同 (上位) |
| `0x15` | `REG_ADC_CTRL` | RW | ADC 制御 (詳細 §5) |
| `0x16` | `REG_ADC_D_L` | R | ADC 値 下位バイト |
| `0x17` | `REG_ADC_D_H` | R | ADC 値 上位バイト (12-bit) |
| `0x1B` | `REG_PWM1_DUTY_L` | RW | PWM ch0 デューティ下位 |
| `0x1C` | `REG_PWM1_DUTY_H` | RW | PWM ch0 デューティ上位 (bit7=Enable, bit6=Polarity, bit3-0=duty hi) |
| `0x1D` | `REG_PWM2_DUTY_L` | RW | PWM ch1 同 |
| `0x1E` | `REG_PWM2_DUTY_H` | RW | |
| `0x1F` | `REG_PWM3_DUTY_L` | RW | PWM ch2 同 |
| `0x20` | `REG_PWM3_DUTY_H` | RW | |
| `0x21` | `REG_PWM4_DUTY_L` | RW | PWM ch3 同 |
| `0x22` | `REG_PWM4_DUTY_H` | RW | |
| **`0x24`** | **`REG_LED_CFG`** | **RW** | **bit[5:0]=有効 LED 数 (0..32)、bit6=Refresh トリガ、bit7=未使用 (危険?)** |
| `0x25` | `REG_PWM_FREQ_L` | RW | PWM 共通周波数 下位 |
| `0x26` | `REG_PWM_FREQ_H` | RW | PWM 共通周波数 上位 |
| **`0x30`** | **`REG_LED_RAM_START`** | **W** | **LED RAM 起点。LED 1 個 = 2 バイト RGB565 LE、最大 32 個 (= 64 byte)** |

レジスタ 0x18-0x1A, 0x23, 0x27-0x2F は BSP には登場しない (予約 or 未公開)。

---

## 3. GPIO

- **16 ピン構成** (pin 0..15)、ただし割込みは pin 0-13 のみ
- 同じピンに対して direction → pull mode → drive mode → output level → ... の順に
  個別レジスタを書く設計。`_writeBit(reg_l, reg_h, pin, value)` のラッパで
  下位/上位バイトを自動切替

**初期化レシピ** (出力ピンの場合):
```cpp
expander.set_direction(pin, /*output*/true);
expander.digital_write(pin, false);   // 初期 Low
// 必要なら setPullMode / setDriveMode
```

### M5 Stack-chan ベース上のピン割当て (実機確認済)

| ピン | 用途 |
|---|---|
| `0` (`kPinServoPowerEnable`) | サーボ電源 EN (High = ON、200 ms 後にサーボ通信可能) |
| `14` | WS2812 NeoPixel データ ライン (本体 12 個 + Hat の追加分)、PY32 内部で自動制御。ホスト側は § 7 の RAM/Refresh のみ操作 |

その他のピンは Stack-chan ベース上では未配線または用途不明。

---

## 4. ADC

- 4 チャンネル (channel 1..4)、12-bit (0..4095)
- ワンショット モード: `REG_ADC_CTRL` に start bit (bit6) + channel (bit2:0) を書く
  → busy bit (bit7) がクリアされるまで polling → `REG_ADC_D_L/H` から値を読む
- 1 回の変換が `< 10 ms` で完了する想定 (BSP の poll は 100 回 / 単純 busy ループ)

```cpp
// channel: 1..4
writeRegister8(REG_ADC_CTRL, (1 << 6) | (channel & 0x07));
while (readRegister8(REG_ADC_CTRL) & (1 << 7)) { /* delay */ }
uint16_t value = readRegister8(REG_ADC_D_L) | (readRegister8(REG_ADC_D_H) << 8);
```

---

## 5. PWM

- 4 チャンネル (channel 0..3)、12-bit デューティ (0..4095)
- 共通周波数 (4 チャンネルで 1 つの `REG_PWM_FREQ_L/H`)
- デューティ上位レジスタ (`_DUTY_H`) の **bit7 = Enable**、**bit6 = Polarity**、
  下位 4 ビットがデューティの上位 4 ビット
- BSP の `setPwmDuty` は 0-255 入力 を 0-4095 にスケール (`duty * 16`)、
  bit7 (Enable) を常に立てる

---

## 6. LED ストリップ (WS2812 NeoPixel) — 本リポジトリの主要関心事

PY32 が WS2812 のタイミング クリティカルなビット叩きを担当し、ホストは
ただ RAM に色データを書いて refresh を立てるだけ。GPIO bit-bang や
RMT を ESP32 側で行う必要はない。

### 6.1 容量

- **最大 32 LED**
- 1 LED = **2 バイト RGB565 little-endian** (= 全体で最大 64 バイト)
- M5 Stack-chan ベースに実装されているのは **12 LED** (背面)

### 6.2 RGB565 ビット レイアウト

```
hi byte (offset 1):  RRRRR GGG    (R 5 bits, G 上位 3 bits)
lo byte (offset 0):  GGG BBBBB    (G 下位 3 bits, B 5 bits)
```

つまり LED1 個分の 2 バイトは `[lo, hi]` の順、I2C 書込みでも同じ順:

```
write_register_block(REG_LED_RAM_START + index*2, [lo, hi])
```

RGB888 → RGB565 変換 (BSP 流):
```cpp
uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
```

### 6.3 操作シーケンス

```
1. set_led_count(N)      → REG_LED_CFG[5:0] = N (0..32)
                            ※ bit6 (refresh) と bit7 を保持したまま下位 6 bit のみ更新が安全
2. write_led_colors(buf) → REG_LED_RAM_START から 2*N バイトを burst write
                            (RGB565 LE pair × N)
3. refresh_leds()        → REG_LED_CFG の bit6 を立てる (read-modify-write)
                            → PY32 が次フレームで NeoPixel 線に出力
                            ※ bit6 は PY32 が自動でクリアする
```

set_led_count(0) で消灯。

### 6.4 旧実装の致命的バグ (備忘)

リポジトリ コミット `f5fa471` 時点の `Py32Expander::write_led_colors` は
「1 LED = **3 バイト** GRB888 順 (WS2812 線出力順)」と誤解していた。
これは PY32 firmware の実プロトコル (= **2 バイト RGB565 LE**) と不一致のため、
書き込みが意味を成さず LED は変化しなかった。さらに当時行った I2C scan
(全アドレス probe write) が AXP2101 (`0x34`) の内部設定を破壊し、LCD
バックライトを消す副作用も併発した。

**教訓**: PY32 の LED RAM は **WS2812 線出力順ではなく** RGB565 LE。M5 BSP
の公式実装が一次仕様、empirical な逆解析は (LED が反応していない時点で)
信頼してはいけない。

---

## 7. 識別と存在検出

`begin()` は `REG_VERSION` を読み、`0x00` でも `0xFF` でもなければ
プローブ成功とみなす。これにより I2C scan を使わずに「PY32 が居るかどうか」
を判定できる。本リポジトリでも `Py32Expander::probe()` が同じパターンを
採用しており、M5 base / Takao base の検出に使っている。

`readDeviceUID()` で 16-bit の UID も取れるが、用途は不明 (個体識別?)。

---

## 8. M5Unified 内蔵ドライバとの関係

M5Unified 本家リポジトリには **PY32 用ドライバは含まれていない** (StackChan-BSP
にのみ存在)。本リポジトリでは `components/board/io_expander_py32.{hpp,cpp}` で
最小限の自前実装を持ち、サーボ電源 EN (pin 0) と LED ストリップ (§ 6) のみ
カバーしている。

`IOExpander_Base` (M5Unified 提供) は GPIO の方向/出力/プル/ドライブ/IRQ 操作の
仮想インタフェース。本リポジトリの実装はこれを継承せず、必要な機能だけ
切り出した薄いラッパに留めている。

---

## 9. 既知の不明点 / 確認 TODO

- `REG_LED_CFG` bit7 の用途 — BSP も触っていない。誤って書込むと挙動不明
  (LED OFF か、PY32 内部状態リセット等の可能性)。安全策として **読み出して
  bit7 と bit6 (refresh trigger) を保持** したまま LED 数だけ更新するべき
- `REG_LED_CFG` の bit6 (refresh) はワンショットらしく PY32 が自動クリアする
  と推定。連続 refresh する場合は毎回 read-modify-write
- レジスタ `0x27-0x2F` (LED CFG と RAM 開始の間) の用途は不明
- 100 kHz / 400 kHz の最大速度は未確認
- WS2812 線への出力タイミング (PY32 内蔵の固定パラメータ?)

---

## 10. 参考

- 上流 BSP: `~/repos/StackChan-BSP/src/drivers/PY32IOExpander/` (read-only / MIT)
- 本リポジトリの抜粋実装: [`components/board/io_expander_py32.{hpp,cpp}`](../components/board/io_expander_py32.hpp)
- M5Stack Stack-chan 製品ページ (回路図は非公開):
  <https://shop.m5stack.com/products/stack-chan-cores3-takao-base-kit>
