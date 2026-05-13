# stackchan-idf

M5Stack CoreS3 向けスタックチャン ファームウェア (ESP-IDF 5.4 / C++20)。

## ライセンス

このリポジトリのソース (`components/board`, `components/scs_servo`,
`components/avatar`, `main`, `tools`) は **Boost Software License 1.0**
の下で配布されます ([LICENSE](LICENSE) 参照)。Submodule
(M5GFX / M5Unified / tl_expected/expected) と managed_components は
それぞれのアップストリーム ライセンスに従います。

新規ソース ファイルには下記の SPDX ヘッダを付与してください:

```cpp
// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
```

## ビルド

```sh
git submodule update --init --recursive
tools/apply-m5-patches.sh                 # M5Unified の 1 行修正を当てる
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`components/M5Unified` / `components/M5GFX` / `components/tl_expected/expected`
は upstream を指す submodule。M5Unified `RTC_PowerHub_Class::setAlarmIRQ` の
`buf` 初期化未指定が GCC `-Werror=maybe-uninitialized` に引っかかるため、
[patches/m5unified.patch](patches/m5unified.patch) を
[tools/apply-m5-patches.sh](tools/apply-m5-patches.sh) で適用する。

## コーディング規約

- 言語: **C++20**。各コンポーネントの `CMakeLists.txt` で
  `target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)` を必ず宣言。
- **エラー伝搬は `tl::expected`** ([components/tl_expected/expected](components/tl_expected/expected)、
  `#include <tl/expected.hpp>`)。例外は使わない。C++20 では `std::expected` が無いので backport を使用。
- 値の有無は `std::optional`。
- **非 null の参照渡しはポインタではなく `T&`**。
- 所有は `std::unique_ptr` / `std::shared_ptr`。生ポインタは非所有 view のみ。
- ヒープ上のバイトバッファは `std::vector<std::uint8_t>`、
  固定長スタック バッファは `std::array<std::uint8_t, N>`。
- range-based for / イテレータを積極利用。
- 整数型は `<cstdint>` の `std::uint*_t` / `std::int*_t`。配列添字は `std::size_t`。
- フォーマット: ルートの [.clang-format](.clang-format) (LLVM 派生 / 4-space / 120 cols)。
- M5Unified を使う。Arduino `Wire` / `Serial` の生 API は避け、IDF 6.0 の
  `i2c_master` / `esp_driver_uart` を直接使う方が望ましい。

## コンポーネント境界

| コンポーネント | 役割 |
|---|---|
| `components/board` | CoreS3 HW 初期化 (`M5.begin()` + PY32 IO Expander でサーボ電源制御) |
| `components/scs_servo` | SCS0009 サーボ ドライバ + 台形速度プロファイル `PathGenerator` |
| `components/avatar` | 顔描画 + アニメーション (Breath / Saccade / Blink、6 表情) |
| `main/` | タスク起動 (Render Task / Servo Task) と共有状態 |

Avatar / Servo は **必ず別コンポーネントに切り出す** (PLAN.md 要求)。

## サーボ電源シーケンス

`board.set_servo_power(false)` 起動 → I2C 経由で PY32 (0x6F) Pin 0 を Low →
ON にする時は `set_servo_power(true)` を呼んで **200 ms 待ってから** `ScsBus`
を使う (バス電圧が立ち上がるまでサーボは応答しない)。

## 参照プロジェクト (READ ONLY / コード流用禁止)

設計の参考に読むことはあるが、コード自体を持ち込まないこと。

- `~/repos/tab5_claude_client` — ESP-IDF 6.0 の正準的レイアウト
- `~/repos/stackchan-rs` — Rust 版 CoreS3 実装 (`PathGenerator` 設計など)
- `~/repos/m5stack-avatar-rs` — Rust 版アバター描画
- `~/repos/scs-servo-rs` — SCS0009 プロトコル
- `~/repos/StackChan-BSP` — CoreS3 HW 初期化シーケンス (FTServo 等のドライバ コードは流用しない)

## ハードウェア定数

- 内部 I2C
  - PMIC AXP2101: 0x34 (M5Unified が管理)
  - Touch: 0x38 / 0x48 系 (M5Unified が管理)
  - PY32 IO Expander: **0x6F** (Pin 0 = サーボ VM 電源 EN)
- サーボ バス (SCS0009)
  - **UART1**, TX = GPIO6, RX = GPIO7, **1 Mbps**, 8N1
  - Yaw: ID = 1, zero_pos = 460
  - Pitch: ID = 2, zero_pos = 620
  - 1 step ≈ 0.3125° (`deg = (raw - zero) * 5 / 16`)

## プロジェクト固有メモ

- `PLAN.md` は `.gitignore` 済み。ローカル設計メモ用。
