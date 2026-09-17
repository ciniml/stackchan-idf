# 第三者ソフトウェア・データの帰属表示 (Third-Party Notices)

stackchan-idf の**自前ソース**は Boost Software License 1.0 ([LICENSE](LICENSE))
で配布されます。本ファイルは、firmware / Web フラッシャー配布物 (リリース ZIP・
GitHub Pages) に含まれる、または同梱する**第三者のソフトウェアと音声データ**の
帰属表示と適用ライセンスをまとめたものです。

このページの HTML 版は <https://ciniml.github.io/stackchan-idf/licenses.html>
から参照できます (設定ページ・Web フラッシャーからもリンク)。

---

## 音声合成エンジン

### hts_engine API 1.10 — Modified BSD (3-clause BSD)

HMM 音声合成エンジン。`components/hts_engine/` に組み込み向け改変コピーを同梱
(改変点は [components/hts_engine/README.md](components/hts_engine/README.md) 参照)。

- Copyright (c) 2001-2015 Nagoya Institute of Technology, Department of Computer Science
- Copyright (c) 2001-2008 Tokyo Institute of Technology, Interdisciplinary Graduate School of Science and Engineering
- 上流: <https://hts-engine.sourceforge.net/>
- ライセンス全文: [components/hts_engine/COPYING](components/hts_engine/COPYING)

> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met
> ... THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
> "AS IS" ...

---

## 音声モデル (HMM ボイス)

以下は firmware にバンドルするか、Web フラッシャー / 設定ページの「サーバーから
取得」で配布するボイス データです。いずれも Creative Commons 表示ライセンスで、
帰属表示のうえ再配布しています。マニフェスト
[assets/voices.json](assets/voices.json) にも各ボイスの帰属文字列を持たせ、設定
ページのボイス選択 UI に表示します。

### HTS Voice "Mei" — CC BY 3.0

配布ボイス (既定候補)。`assets/voices/mei.htsvoice` として同梱し、GitHub Pages
の `voices/mei.htsvoice` から配布します。

- HTS Voice "Mei" © 2009-2013 Nagoya Institute of Technology / MMDAgent Project Team
- License: [Creative Commons Attribution 3.0](https://creativecommons.org/licenses/by/3.0/)
- ライセンス全文: [assets/voices/LICENSE_mei.txt](assets/voices/LICENSE_mei.txt)
- 上流: <http://www.mmdagent.jp/>
- **改変版**: `assets/voices/mei16.htsvoice` は上記 Mei を **16 kHz にオフライン
  ダウンサンプル**した派生物 (帯域制限メルケプストラム変換、Kenta IDA 2026)。
  CC BY 3.0 に基づき改変を明示。合成負荷を下げる高速版として同カタログで配布。

### その他の CC BY ボイス (ユーザーがアップロードして利用可能)

firmware は任意の `.htsvoice` を読み込めます。以下はライセンス上クリーンで
検証に用いたボイス (リポジトリには同梱しません)。利用・再配布時は各ライセンスの
帰属表示に従ってください。

- **htsvoice-tohoku-f01** (女声) — CC BY 4.0, © 東北大学 乾・鈴木研究室 (icn-lab)
- **hts_voice_nitech_jp_atr503_m001** (男声) — CC BY 3.0, © Nagoya Institute of Technology

---

## テキスト解析 (ホスト側ツールのみ)

`tools/jvox/`・`docs/jtts-hmm-research.md` の音声データ生成・検証は、ホスト上で
Open JTalk / pyopenjtalk を使います (firmware には含みません)。

- **Open JTalk / hts_engine / pyopenjtalk** — Modified BSD 等。生成に用いた
  かな→ラベル変換のリファレンスであり、成果物 (.htsvoice / .jvox) の
  ライセンスは元の音声モデルのライセンスに従います。

---

## ライブラリ (submodule / managed components)

| コンポーネント | ライセンス | 著作権表示 |
|---|---|---|
| [M5GFX](third_party/m5gfx/LICENSE) | MIT | © 2021 M5Stack |
| [M5Unified](components/M5Unified/LICENSE) | MIT | © 2021 M5Stack |
| [tl::expected](components/tl_expected/expected/COPYING) | CC0 1.0 (public domain) | Sy Brand |
| ESP-IDF managed_components (`espressif/*`) | Apache-2.0 | © Espressif Systems |

## Web フラッシャー (GitHub Pages)

| コンポーネント | ライセンス | 著作権表示 |
|---|---|---|
| esptool-js (`docs/esptool.js`) | Apache-2.0 | © Espressif Systems |
| pako (esptool-js に同梱) | MIT AND Zlib | © 2014-2017 Vitaly Puzrin, Andrey Tupitsin |

---

各ライセンスの全文は上記リンク先、および各コンポーネントの LICENSE / COPYING
ファイルを参照してください。
