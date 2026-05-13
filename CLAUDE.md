# stackchan-idf

M5Stack CoreS3 向けスタックチャン ファームウェア (ESP-IDF 6.0 / C++23)。

## ビルド

```sh
git submodule update --init --recursive
tools/apply-m5-patches.sh                 # IDF 6 / CoreS3 patches を当てる
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`components/M5Unified` / `components/M5GFX` は upstream `master` を指す
submodule。IDF 6.0 / CoreS3 で動作させるため、[patches/](patches/) の
ローカルパッチを [tools/apply-m5-patches.sh](tools/apply-m5-patches.sh) で
適用する (再 sync 手順含めて [M5_IDF6_PATCHES.md](M5_IDF6_PATCHES.md) を参照)。

## コーディング規約

- 言語: **C++23**。各コンポーネントの `CMakeLists.txt` で
  `target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)` を必ず宣言。
- **エラー伝搬は `std::expected`**。例外は使わない。
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
