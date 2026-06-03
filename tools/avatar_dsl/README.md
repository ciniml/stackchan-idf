<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# Avatar DSL

Lua 風 imperative 言語 → `AVDS` バイトコード コンパイラ。Stack-chan ファームの
`components/avatar_vm` がこのバイトコードを実行してアバターの顔を描画する。
Node 18+ で動く CLI と、`<script type="module">` から直接 import できる
ブラウザ実装が同じソースから動く (ESM)。

## なぜ DSL なのか

旧版は `components/avatar/{face,eye,eyebrow,mouth,effect}.cpp` で **C++ ハード
コードされた描画処理**だったため、表情・パーツの座標・色・条件分岐を変えるたびに
ファーム再ビルドが必要だった。バイトコードに切替えたことで:

1. `POST /api/avatar-dsl` (本体の Wi-Fi 設定エンドポイント) に `.avbc` を送る
   だけで顔が即時 差し替わる
2. デフォルト顔は `assets/default_face.avdsl` を CI でコンパイルしたものが
   firmware に embed される (ソースは Git で差分管理可能)
3. ブラウザのライブ プレビュー (`tools/settings.html` の avatar_module) も
   同じバイトコードを走らせるので、編集 → ブラウザ確認 → 送信 のループが
   ピクセル等価で回る

## 文法 — Lua 風 imperative

```lua
-- コメントは `-- ` から行末まで
-- リテラル: 1.5, 100, 0xFFFF, true, false
-- 識別子: snake_case
-- 演算子 (優先度 低→高):
--   or
--   and
--   == != ~=                -- `~=` は bool XOR (Lua の `~=` を流用)
--   < <= > >=
--   + -
--   * / %
--   unary -  /  not
-- ビルトイン定数 (コンパイラが値を埋め込み):
--   NEUTRAL=0 HAPPY=1 SAD=2 ANGRY=3 DOUBT=4 SLEEPY=5  -- 表情 enum
--   true=1 false=0

-- ユーザー関数
fn helper(p1, p2)
  let local_var = p1 + p2 * 2          -- 局所変数 (再代入可能)
  if local_var > 100 then
    return                              -- 早期 return (引数なし)
  end
end

-- 制御構文
if cond then ... elif cond then ... else ... end
while cond do ... end                   -- 無限ループ防止策は無し、責任は書き手

-- 描画 builtins (戻り値なし):
--   fill_rect(x, y, w, h, color)
--   fill_circle(cx, cy, r, color)
--   fill_triangle(x0, y0, x1, y1, x2, y2, color)
--   begin_group(x, y, w, h) / end_group()
-- 計算 builtins (戻り値あり):
--   min(a, b), max(a, b), clamp(v, lo, hi)
--   abs(x), floor(x), round(x), sqrt(x)
--   sz(s)  -- max(1, s * canvas_scale)  (描画サイズの最小 1px 保証)
--   tx(bx) -- canvas_w/2 + (bx - 160) * canvas_scale   (320×240 設計座標を実画面 X に)
--   ty(by) -- canvas_h/2 + (by - 120) * canvas_scale   (同上 Y)

-- read-only コンテキスト変数 (ホストから注入):
--   canvas_w canvas_h canvas_scale now_ms
--   breath eye_open gaze_h gaze_v mouth_open    -- 0..1 / -1..1
--   expr                                          -- enum 0..5
--   primary background secondary balloon_fg balloon_bg   -- RGB565
--   eye_radius eye_off_x eye_off_y
--   brow_off_x brow_off_y
--   mouth_off_x mouth_off_y
--   mouth_min_w mouth_max_w mouth_min_h mouth_max_h
--   eyebrows_visible

-- エントリ ポイント (必須・引数なし)
fn draw()
  fill_circle(tx(160), ty(120), sz(eye_radius), primary)
end
```

詳細な例は [assets/default_face.avdsl](../../assets/default_face.avdsl) を参照。

## バイトコード ファイル形式 (`.avbc`)

```
Header (16 bytes, all little-endian):
  0x00  magic "AVDS"        (4 bytes)
  0x04  version             (u16 = 1)
  0x06  flags               (u16, reserved 0)
  0x08  const_count         (u16)
  0x0A  fn_count            (u16)
  0x0C  code_size           (u16, bytes)
  0x0E  entry_fn_id         (u16, 0 = `fn draw()`)
Const table: const_count × (1 byte tag + payload)
  tag 0x01: f32 (4 bytes)
  tag 0x02: i32 (4 bytes)
  tag 0x03: RGB565 color (2 bytes)
Fn table:   fn_count × 6 bytes
  u16 code_offset, u8 param_count, u8 local_count, u16 reserved
Code: code_size bytes (JMP / Jz / Jnz オフセットの基準はこのセクション内)
```

オペコード/var ID の真実源は [components/avatar_vm/include/avatar_vm/opcodes.hpp](../../components/avatar_vm/include/avatar_vm/opcodes.hpp)
([tools/avatar_dsl/opcodes.js](opcodes.js) が JS 側ミラー)。スタック VM、
値はすべて `float32` 単一型。

## 使い方

### Node CLI (ビルド時 / 手動)

```sh
node tools/avatar_dsl/cli.mjs assets/default_face.avdsl /tmp/out.avbc
```

ファーム ビルド (`make build`) は CMake (`components/avatar_vm/CMakeLists.txt`)
が自動でこれを呼び、生成バイトコードを `EMBED_FILES` でファームに埋める。
WASM プレビュー ビルド (`wasm/build.sh`) も同じ CLI を起動して
`default_face_avbc.h` (バイト配列) を生成 → emcc に渡す。

### ブラウザ

```html
<script type="module">
  import { compile } from './tools/avatar_dsl/compile.js';
  const src = document.getElementById('dsl').value;
  let buf;
  try { buf = compile(src); }   // ArrayBuffer
  catch (e) { showError(e.message); return; }
  await fetch('http://stackchan-XXXXXX.local/api/avatar-dsl',
              { method: 'POST', body: buf });
</script>
```

成功時 200 + `{"ok":true,"saved":N,"live":true}` が返る (live=false は
NVS には書けたが render task のホット スワップが失敗 — 通常起き得ない)。

ライブ プレビューは `wasm/avatar_wasm.cpp` の `avatar_load_bytecode(ptr, len)`
を `ccall` で呼べばその場で差し替わる (バイト配列を WASM heap に書いてから
渡す)。

### デフォルトへ戻す

```sh
curl -X POST http://stackchan-XXXXXX.local/api/avatar-dsl/reset
```

NVS からブロブを削除 + 起動時に embed されている default を再ロード
(ハード リブート不要)。

## 制限事項

- スタック深度 64、最大ローカル 256 (関数あたり)、最大関数数 256、
  コード セクション 64 KiB
- 数値型は `f32` のみ。色は `f32` で運び描画 API 投入時に `u16` キャスト
- 文字列リテラルとテキスト描画は未対応 (balloon は引き続き C++ 実装)
- `fill_round_rect` / `draw_round_rect` は予約 opcode (0x43/0x44) として
  空けてあるが未実装
- ループは `while` のみ。`for`、配列、テーブル、クロージャは無し
- 浮動小数点除算で 0 を割ると `VmError::DivideByZero` で実行停止
  (= 1 フレーム分の描画が中断、次フレームから再試行)

## トラブルシュート

| 症状 | 原因の可能性 |
|---|---|
| `bad magic` | 別ファイル / 別バージョンの `.avbc` を送っている |
| `bad version` | コンパイラが古い、または firmware がこの形式に未対応 |
| ホット スワップ後に表情が変わらない | ホット スワップ自体は適用済み (NVS にも書かれた)、描画ループの判定式が分岐していないだけ。`expr` 変数を読んでいるか確認 |
| パーツの位置がズレる (CoreS3 では合うが AtomS3R で見切れる) | 設計座標を実画面に写すラッパー (`tx/ty/sz`) を通していない |
| `fill_circle` で表情変化のたびに古い円が残る | `begin_group` の bbox に最大変化レンジが入っていない (Direct Canvas 戦略の前提) |

詳細仕様 [docs/avatar_dsl_spec.md](../../docs/avatar_dsl_spec.md) も参照。
