<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# Stack-chan Avatar DSL — 仕様 / サンプル / バイトコード

> このページは `stackchan-idf` v0.4.0 以降に同梱される **アバター描画 DSL** の
> 公開仕様。

ファームウェアのアバター顔は、Lua 風 imperative 言語で書かれた小さな
**描画 DSL** をコンパイル → バイトコード化 → 内蔵 VM で実行することで描かれる。
従来は C++ ハードコードだった顔描画を、再ビルド無しで差し替えられるように
した枠組み。

- **ソース言語**: 小さな Lua サブセット (関数 / 局所変数 / `if/while`)
- **バイトコード**: スタック VM、1 バイト オペコード、float32 単一型
- **コンパイラ**: `tools/avatar_dsl/` の JavaScript 実装。Node CLI と
  ブラウザ inline の両方で動く
- **エンジン**: ファーム内 `components/avatar_vm/` (C++20 / `tl::expected`)、
  WASM プレビュー (`avatar_module.js`) でも同じ VM が動く
- **配信**: Wi-Fi 経由の `POST /api/avatar-dsl` で NVS に保存 → 即適用
- **デフォルト顔**: [assets/default_face.avdsl](https://github.com/ciniml/stackchan-idf/blob/main/assets/default_face.avdsl)
  (ビルド時にコンパイルされ firmware に embed)

---

## 1. クイックスタート

### ブラウザ プレビュー (`avatar.html`)

[Web プレビュー](https://ciniml.github.io/stackchan-idf/avatar.html) を開くと
画面右側に「顔描画 DSL — 編集して即時反映」エディタがある。

1. テキスト エリアの内容を編集
2. **コンパイル & 適用** (または `Ctrl/Cmd + Enter`) で即時 反映
3. 気に入ったら **.avbc ダウンロード** でバイトコードを保存
4. 実機へは:
   ```sh
   curl --data-binary @face.avbc \
        -X POST http://stackchan-XXXXXX.local/api/avatar-dsl
   # → {"ok":true,"saved":<N>,"live":true}
   ```
5. デフォルトに戻すには:
   ```sh
   curl -X POST http://stackchan-XXXXXX.local/api/avatar-dsl/reset
   ```

実機 NVS に保存されるので、書き込んだ顔は再起動後も維持される。

### 最小の Hello

```lua
fn draw()
  fill_circle(tx(160), ty(120), sz(40), primary)
end
```

中心に白い円が 1 つ描かれるだけのファイル (約 30 バイトのバイトコード)。

---

## 2. 文法

### 2.1 構文

```
program     := fn_decl+
fn_decl     := 'fn' IDENT '(' params? ')' block 'end'
params      := IDENT (',' IDENT)*
block       := stmt*
stmt        := let_stmt | assign_stmt | if_stmt | while_stmt
             | return_stmt | expr_stmt
let_stmt    := 'let' IDENT '=' expr            -- 局所変数を新規作成 or 値再書込み
assign_stmt := IDENT '=' expr                  -- 既存局所変数への再代入
if_stmt     := 'if' expr 'then' block
               ('elif' expr 'then' block)*
               ('else' block)?
               'end'
while_stmt  := 'while' expr 'do' block 'end'
return_stmt := 'return'                        -- 戻り値は持たない
expr_stmt   := expr                            -- 通常は関数呼び出し
expr        := ...                             -- §2.3 演算子優先順位 参照
```

### 2.2 字句

- **コメント**: `-- 行末まで`
- **識別子**: `[a-zA-Z_][a-zA-Z0-9_]*` — `snake_case` を推奨
- **キーワード (予約語)**:
  `fn end let if then elif else while do return and or not true false`
- **数値リテラル**:
  - 10 進整数: `42`、`-1`
  - 16 進整数: `0xFFFF` (大文字小文字いずれも可)
  - 浮動小数: `1.5`、`.5`、`1e3`、`1.5e-2`
  - bool: `true` / `false` (内部表現は `1.0` / `0.0`)
- **演算子記号**: `+ - * / % == != ~= < <= > >= = ( ) ,`
  - `~=` は **bool XOR** (Lua の "not equal" とは別の意味。本 DSL では `!=` が
    not-equal、`~=` はブール排他的論理和)
- **文字列リテラル**: **未対応** (テキスト描画は将来予約)

### 2.3 演算子優先順位

低 → 高:

| 優先度 | 演算子 | 結合性 | 説明 |
|---|---|---|---|
| 1 | `or` | 左 | 論理和 (短絡評価) |
| 2 | `and` | 左 | 論理積 (短絡評価) |
| 3 | `== != ~=` | 左 | 等価 / bool XOR |
| 4 | `< <= > >=` | 左 | 比較 |
| 5 | `+ -` | 左 | 加減算 |
| 6 | `* / %` | 左 | 乗除剰余 |
| 7 | 単項 `-`, `not` | 右 | 符号反転 / 論理否定 |
| 8 | 関数呼び出し / 識別子 / 数値 / `()` | — | 一次式 |

論理演算子は短絡: `cond and expr` は `cond` が偽の場合 `expr` を評価しない。
比較・論理の結果は `0.0` または `1.0` の float。

### 2.4 関数

- 全関数は **`fn name(params) ... end`** で宣言。
- **`fn draw()`** が必須・引数なしのエントリ ポイント (バイトコードの
  `entry_fn_id = 0` に割当てられる)。
- ユーザー関数は戻り値を持たない (`return` は早期 return のみ、値は積めない)。
- 引数は左から右に評価されてスタック→ローカル スロット 0..N-1 に格納。
- 関数あたり最大 **256 局所スロット** (引数含む)。
- 関数総数は最大 **256**。

### 2.5 局所変数

- `let name = expr` で新規作成、または既存名への代入。
- 関数スコープ。ブロック スコープは無い (`if` 内の `let` も関数末まで生存)。
- 再代入は `name = expr` (let を付けずに)。未宣言名への代入はコンパイル エラー。
- 同名の context 変数を `let` で隠せる (シャドウイング許可)。

### 2.6 関数呼び出し

```
call := IDENT '(' (expr (',' expr)*)? ')'
```

- ビルトイン (`fill_rect`, `tx`, ...) と ユーザー定義関数を同じ構文で呼び出す。
- 描画系ビルトインは式文 (`fill_rect(...)`) として、計算系は式の中で使う。
- 引数の数はコンパイル時にチェックされる。

---

## 3. 標準ビルトイン

### 3.1 計算 (戻り値あり)

| 名前 | 引数 | 説明 |
|---|---|---|
| `min(a, b)` | 2 | 数値最小 |
| `max(a, b)` | 2 | 数値最大 |
| `clamp(v, lo, hi)` | 3 | `v` を `[lo, hi]` にクランプ |
| `abs(x)` | 1 | 絶対値 |
| `floor(x)` | 1 | 床関数 |
| `round(x)` | 1 | 四捨五入 (HALF_TO_EVEN ではなく `std::round`) |
| `sqrt(x)` | 1 | 平方根 |
| `sz(s)` | 1 | `max(1, s * canvas_scale)` — 描画サイズの最小 1px 保証 |
| `tx(bx)` | 1 | 設計座標 (320×240, 中心 160,120) → 実画面 X |
| `ty(by)` | 1 | 同 → 実画面 Y |

`tx/ty/sz` を使えば、320×240 で書いた座標が CoreS3 (320×240) でも
AtomS3R (128×128) でも自動的にスケール (0.4×) され、中央寄せで描画される。

### 3.2 描画 (戻り値なし)

| 名前 | 引数 | 説明 |
|---|---|---|
| `fill_rect(x, y, w, h, color)` | 5 | 塗りつぶし矩形 |
| `fill_circle(cx, cy, r, color)` | 4 | 塗りつぶし円 |
| `fill_triangle(x0,y0,x1,y1,x2,y2,color)` | 7 | 塗りつぶし三角形 |
| `begin_group(x, y, w, h)` | 4 | グループ開始 (§4 参照) |
| `end_group()` | 0 | グループ終了 |

座標は `int16_t` にキャストされて描画 API に渡る。
色は `uint16_t` (RGB565) にキャスト。

### 3.3 グループ (重要)

`begin_group(x, y, w, h)` / `end_group()` は、多プリミティブで構成される
パーツ (目・口・effect など) を「変化レンジを覆う bbox」で囲むためのもの。

- **バッファ戦略** (CoreS3 など PSRAM 有) では `begin_group`/`end_group` は
  **no-op** (どうせ毎フレーム全画面クリア + 全描画なので)。
- **Direct 戦略** (AtomS3R など PSRAM 無) では、bbox 内のみを背景でクリア
  してから内部プリミティブを SRAM スプライトに描き、終端で panel に blit する。
  bbox を**過小に指定すると前フレームのゴーストが残る**ので、変化レンジを
  最大限カバーする必要がある。

例 (目: 半径 ± 16px の正方形で囲む):
```lua
begin_group(cx - r - 16, cy - r - 16, (r + 16) * 2, (r + 16) * 2)
  fill_circle(cx + gaze_h * 3, cy + gaze_v * 3, r, primary)
end_group()
```

---

## 4. コンテキスト変数 (read-only)

すべて float として読み取れる。動的な値は `tick()` 直前に Animator が更新。

| 名前 | 型 | 範囲 | 説明 |
|---|---|---|---|
| `canvas_w`, `canvas_h` | int | 64..1280 | 実画面ピクセル数 |
| `canvas_scale` | float | (0, ∞) | `min(canvas_w/320, canvas_h/240)` |
| `now_ms` | u32 → float | — | フレーム時刻 (ms) |
| `breath` | float | 0..1 | 呼吸位相 |
| `eye_open` | float | 0..1 | まばたき (0 = 閉) |
| `gaze_h`, `gaze_v` | float | -1..+1 | 視線サッカード |
| `mouth_open` | float | 0..1 | 口の開き |
| `expr` | enum | 0..5 | 表情 (下記定数で名前指定可) |
| `primary` | u16 → float | RGB565 | 前景色 (デフォルト 白 `0xFFFF`) |
| `background` | u16 → float | RGB565 | 背景色 (デフォルト 黒 `0x0000`) |
| `secondary` | u16 → float | RGB565 | 予約色 (デフォルト 黄 `0xFFE0`) |
| `balloon_fg`, `balloon_bg` | u16 → float | RGB565 | バルーン色 |
| `eye_radius` | float | 1..20 | 目の半径 (FaceTuning) |
| `eye_off_x`, `eye_off_y` | float | -40..40 | 目のオフセット (左右対称適用) |
| `brow_off_x`, `brow_off_y` | float | -40..40 | 眉のオフセット |
| `mouth_off_x`, `mouth_off_y` | float | -60..60 | 口のオフセット |
| `mouth_min_w`, `mouth_max_w` | int → float | 4..200 | 口幅レンジ |
| `mouth_min_h`, `mouth_max_h` | int → float | 1..120 | 口高レンジ |
| `eyebrows_visible` | bool | 0/1 | 眉表示フラグ |

### 表情列挙 (コンパイル時に値展開される定数)

| 識別子 | 値 |
|---|---|
| `NEUTRAL` | 0 |
| `HAPPY` | 1 |
| `SAD` | 2 |
| `ANGRY` | 3 |
| `DOUBT` | 4 |
| `SLEEPY` | 5 |

---

## 5. サンプル

### 5.1 表情で色を変える単純な顔

```lua
fn draw()
  let face_color = primary
  if expr == ANGRY then face_color = 0xF800   -- 赤
  elif expr == HAPPY then face_color = 0xFFE0  -- 黄
  elif expr == SLEEPY then face_color = 0x4208 -- 暗い灰
  end

  begin_group(0, 0, canvas_w, canvas_h)
  -- 目 2 個
  fill_circle(tx(110), ty(110), sz(12), face_color)
  fill_circle(tx(210), ty(110), sz(12), face_color)
  -- 口 (mouth_open で開く)
  let mh = sz(4 + mouth_open * 30)
  let mw = sz(60)
  fill_rect(tx(160) - mw / 2, ty(170) - mh / 2, mw, mh, face_color)
  end_group()
end
```

### 5.2 視線追従の瞳孔

`gaze_h/gaze_v` (-1..+1) を使うと、サッカード/手動視線にあわせて瞳が動く。

```lua
fn eye(cx, cy, r)
  let px = cx + gaze_h * (r * 0.4)
  let py = cy + gaze_v * (r * 0.4)
  begin_group(cx - r - 4, cy - r - 4, (r + 4) * 2, (r + 4) * 2)
  fill_circle(cx, cy, r, background)              -- 白目
  fill_circle(cx, cy, r - 1, primary)              -- 円板
  fill_circle(px, py, round(r * 0.45), background) -- 瞳
  end_group()
end

fn draw()
  eye(tx(110), ty(110), sz(16))
  eye(tx(210), ty(110), sz(16))
end
```

### 5.3 呼吸でパルスする effect

```lua
fn draw()
  let pulse = sz(6 + breath * 4)
  fill_circle(tx(40), ty(40), pulse, primary)
end
```

### 5.4 完全な置換版 (デフォルト顔)

`face/eye/eyebrow/mouth/effect.cpp` を 1 対 1 で写した完全版が
[`assets/default_face.avdsl`](https://github.com/ciniml/stackchan-idf/blob/main/assets/default_face.avdsl)
にある (~ 200 行)。新規顔を書くときの参考に。

---

## 6. バイトコード仕様 (`.avbc` ファイル形式)

すべて **リトル エンディアン**。生成は JS コンパイラ
([`tools/avatar_dsl/compile.js`](https://github.com/ciniml/stackchan-idf/blob/main/tools/avatar_dsl/compile.js))、
デコード + 実行は C++ VM
([`components/avatar_vm/`](https://github.com/ciniml/stackchan-idf/tree/main/components/avatar_vm))。

### 6.1 ヘッダ (16 バイト固定)

| オフセット | サイズ | フィールド | 値 / 意味 |
|---|---|---|---|
| 0x00 | 4 | magic | ASCII `"AVDS"` (= `0x53445641` LE) |
| 0x04 | 2 | version | `1` (今後の破壊的変更時に bump) |
| 0x06 | 2 | flags | 予約、現状 `0` |
| 0x08 | 2 | const_count | 定数表エントリ数 (最大 256) |
| 0x0A | 2 | fn_count | 関数表エントリ数 (最大 256) |
| 0x0C | 2 | code_size | コード セクション バイト数 (≤ 65535) |
| 0x0E | 2 | entry_fn_id | エントリ関数 ID、通常 `0` (= `draw`) |

### 6.2 定数表

`const_count` 個のエントリ。各エントリは可変長:

| タグ | 後続バイト数 | 内容 |
|---|---|---|
| `0x01` F32 | 4 | f32 LE |
| `0x02` I32 | 4 | i32 LE (VM で f32 化) |
| `0x03` Color | 2 | u16 LE (RGB565、VM で f32 化) |
| `0x04` String | (予約) | 将来テキスト描画用 — 現状未実装 |

### 6.3 関数表

`fn_count` 個 × 6 バイト固定:

| オフセット | サイズ | フィールド |
|---|---|---|
| 0 | 2 | `code_offset` (コード セクション内オフセット、LE) |
| 2 | 1 | `param_count` (≤ `local_count`) |
| 3 | 1 | `local_count` (引数 + let 局所変数の総数) |
| 4 | 2 | 予約、`0` |

### 6.4 コード セクション

`code_size` バイト。1 バイト オペコード + 可変オペランド。
`Jmp/Jz/Jnz` のオフセットは "オペランドの直後の PC からの符号付きオフセット"
で計算 (NB: 一般的な ISA と同じセマンティクス)。

### 6.5 オペコード一覧

すべてのスタック値は `float32`。

#### スタック操作

| Op | ニーモニック | オペランド | 意味 |
|---|---|---|---|
| `0x01` | PUSH_F32 | 4B f32 | 即値 push |
| `0x02` | PUSH_I8 | 1B i8 | i8 → f32 push |
| `0x03` | PUSH_I16 | 2B i16 | i16 → f32 push |
| `0x04` | PUSH_CONST | 1B id | 定数表 [id] を push |
| `0x05` | PUSH_VAR | 1B id | コンテキスト変数 push |
| `0x06` | PUSH_LOCAL | 1B slot | ローカル [slot] を push |
| `0x07` | STORE_LOCAL | 1B slot | top を pop してローカル [slot] へ |
| `0x08` | POP | — | top を捨てる |
| `0x09` | DUP | — | top を複製 |

#### 算術 (二項は `pop b; pop a; push f(a,b)`、単項は `pop a; push f(a)`)

| Op | ニーモニック | アリティ | 意味 |
|---|---|---|---|
| `0x10` | ADD | 2 | `a + b` |
| `0x11` | SUB | 2 | `a - b` |
| `0x12` | MUL | 2 | `a * b` |
| `0x13` | DIV | 2 | `a / b` (0 除算で `DivideByZero`) |
| `0x14` | NEG | 1 | `-a` |
| `0x15` | MIN | 2 | `min(a, b)` |
| `0x16` | MAX | 2 | `max(a, b)` |
| `0x17` | ABS | 1 | `|a|` |
| `0x18` | FLOOR | 1 | `floor(a)` |
| `0x19` | ROUND | 1 | `round(a)` |
| `0x1A` | MOD | 2 | `fmod(a, b)` |
| `0x1B` | SQRT | 1 | `sqrt(a)` |
| `0x1C` | CLAMP | 3 | `pop hi, lo, v; push clamp(v,lo,hi)` |
| `0x1D` | SCALE | 1 | `max(1, a * canvas_scale)` |
| `0x1E` | TX | 1 | `canvas_w/2 + (a - 160) * canvas_scale` |
| `0x1F` | TY | 1 | `canvas_h/2 + (a - 120) * canvas_scale` |

#### 比較 / 論理 (結果は `0.0` / `1.0`)

| Op | ニーモニック | アリティ | 意味 |
|---|---|---|---|
| `0x20` | EQ | 2 | `a == b` |
| `0x21` | NE | 2 | `a != b` |
| `0x22` | LT | 2 | `a < b` |
| `0x23` | LE | 2 | `a <= b` |
| `0x24` | GT | 2 | `a > b` |
| `0x25` | GE | 2 | `a >= b` |
| `0x26` | NOT | 1 | 真偽反転 |
| `0x27` | AND | 2 | 両者非ゼロなら 1 |
| `0x28` | OR | 2 | いずれか非ゼロなら 1 |
| `0x29` | XOR | 2 | 片方だけ非ゼロなら 1 |

#### 制御フロー

| Op | ニーモニック | オペランド | 意味 |
|---|---|---|---|
| `0x30` | JMP | 2B i16 | 無条件分岐 (オフセット = オペランド後 PC + i16) |
| `0x31` | JZ | 2B i16 | top が 0 なら分岐 (top は pop) |
| `0x32` | JNZ | 2B i16 | top が非 0 なら分岐 (top は pop) |
| `0x33` | CALL | 1B fn_id | 引数はスタックに積まれている前提で呼出 |
| `0x34` | RET | — | 関数復帰 (root frame の RET で実行終了) |

#### 描画 (引数は宣言順 = 下から上にスタック上にある状態で発火)

| Op | ニーモニック | 消費 | 引数 (積まれる順) |
|---|---|---|---|
| `0x40` | FILL_RECT | 5 | `x, y, w, h, color` |
| `0x41` | FILL_CIRCLE | 4 | `cx, cy, r, color` |
| `0x42` | FILL_TRIANGLE | 7 | `x0, y0, x1, y1, x2, y2, color` |
| `0x43`/`0x44` | (予約) | — | 将来 `fill_round_rect` / `draw_round_rect` 用 |
| `0x45` | BEGIN_GROUP | 4 | `x, y, w, h` |
| `0x46` | END_GROUP | 0 | — |

#### コンテキスト変数 ID

`PUSH_VAR <id>` 用 (1 バイト):

| ID | 名前 | ID | 名前 | ID | 名前 |
|---|---|---|---|---|---|
| 0x00 | `canvas_w` | 0x0A | `primary` | 0x14 | `mouth_off_x` |
| 0x01 | `canvas_h` | 0x0B | `background` | 0x15 | `mouth_off_y` |
| 0x02 | `canvas_scale` | 0x0C | `secondary` | 0x16 | `mouth_min_w` |
| 0x03 | `now_ms` | 0x0D | `balloon_fg` | 0x17 | `mouth_max_w` |
| 0x04 | `breath` | 0x0E | `balloon_bg` | 0x18 | `mouth_min_h` |
| 0x05 | `eye_open` | 0x0F | `eye_radius` | 0x19 | `mouth_max_h` |
| 0x06 | `gaze_h` | 0x10 | `eye_off_x` | 0x1A | `eyebrows_visible` |
| 0x07 | `gaze_v` | 0x11 | `eye_off_y` | | |
| 0x08 | `mouth_open` | 0x12 | `brow_off_x` | | |
| 0x09 | `expr` | 0x13 | `brow_off_y` | | |

> 真実源: [components/avatar_vm/include/avatar_vm/opcodes.hpp](https://github.com/ciniml/stackchan-idf/blob/main/components/avatar_vm/include/avatar_vm/opcodes.hpp)
> (C++ 側) / [tools/avatar_dsl/opcodes.js](https://github.com/ciniml/stackchan-idf/blob/main/tools/avatar_dsl/opcodes.js) (JS 側 ミラー)

### 6.6 実行モデル / リソース制限

- 操作スタック 深さ: **64**
- 関数あたり最大ローカル: **256** (引数含む)
- コール スタック (呼出深さ): **16**
- 関数総数: **256**
- 定数表エントリ数: **256**
- コード セクション サイズ: **65535** バイト
- 0 除算 (`DIV` / `MOD`) は `VmError::DivideByZero` で 1 フレーム描画中断
- 不正オフセット / 不正 ID は `VmError::JumpOutOfBounds` 等で同上
- バイトコード ファイル全体 (NVS 保存) は **32 KiB** が上限

これらを超えた場合は **コンパイル時** (JS) または **デコード時** (C++) に拒否
される。コンパイル後に上限内なら、VM 実行中の崩壊は無い (例外: 0 除算)。

### 6.7 デコード時の検証

`decode()` は次を検証:

- マジック `"AVDS"`、バージョン 1
- 各セクションが宣言サイズ内に収まる (Truncated)
- 定数タグが既知 (BadConstTag)
- entry_fn_id が fn_count 内で、`code_offset < code_size`、`param_count == 0`
- 全 fn の `local_count >= param_count`

実行時に追加でチェック: 未知オペコード (UnknownOpcode)、未知 var/const/fn id、
スタック underflow/overflow、コール スタック超過、ジャンプ範囲外、0 除算。

---

## 7. ワーク フロー: 編集 → 配信

| ステップ | ツール | 出力 |
|---|---|---|
| 1. DSL を書く | エディタ (テキスト) | `face.avdsl` |
| 2. ブラウザ プレビュー | [`avatar.html`](https://ciniml.github.io/stackchan-idf/avatar.html) | 即時 |
| 3. バイトコード化 | ブラウザ「.avbc ダウンロード」 or `node tools/avatar_dsl/cli.mjs face.avdsl face.avbc` | `face.avbc` |
| 4. 実機書込み | `curl -X POST --data-binary @face.avbc http://stackchan-XXXXXX.local/api/avatar-dsl` | 即時適用 + NVS 保存 |
| 5. 戻し | `curl -X POST http://stackchan-XXXXXX.local/api/avatar-dsl/reset` | embedded default に復帰 |

ファーム ビルドに**ビルト イン**したい場合は
[`assets/default_face.avdsl`](https://github.com/ciniml/stackchan-idf/blob/main/assets/default_face.avdsl)
を書き換えて `make build BOARD=<board>` するだけ。CMake が `node cli.mjs` を
呼んで `default_face.avbc` を生成 → `EMBED_FILES` でファームに埋込む。

---

## 8. 既知の制限 / Phase 2 候補

- **文字列 / テキスト描画 なし** — balloon (フキダシ) は引き続き C++ 側で描画
- **`fill_round_rect` / `draw_round_rect` 未実装** (オペコードのみ予約)
- **ループは `while` のみ**、`for`、配列、テーブル、クロージャは無し
- **アルファ合成 / グラデ なし** (M5GFX の RGB565 単色塗り API のみ)
- **BLE 経由のアップロード未対応** (HTTP のみ。BLE chr 追加は次タスク)
- 顔チューニング (`FaceTuning`) と DSL の重複: 現状は両方が context var として
  読めるので二重設定可能 — 将来 DSL に寄せる予定

---

## 9. リファレンス

- ソース: <https://github.com/ciniml/stackchan-idf>
- DSL コンパイラ (JS): [`tools/avatar_dsl/`](https://github.com/ciniml/stackchan-idf/tree/main/tools/avatar_dsl)
- VM (C++): [`components/avatar_vm/`](https://github.com/ciniml/stackchan-idf/tree/main/components/avatar_vm)
- デフォルト顔: [`assets/default_face.avdsl`](https://github.com/ciniml/stackchan-idf/blob/main/assets/default_face.avdsl)
- ブラウザ プレビュー: <https://ciniml.github.io/stackchan-idf/avatar.html>
- Web フラッシャ: <https://ciniml.github.io/stackchan-idf/>
- 設定ページ: <https://ciniml.github.io/stackchan-idf/settings.html>

ライセンス: BSL-1.0 (リポジトリと同じ)
