# sanoTTS-jp 組み込み方針 (検討メモ)

- 日付: 2026-09-15
- 状態: 方針確定 (2026-09-15)、実装は §4 の順に進行中
- 参照: 公式リポジトリ https://github.com/ayutaz/sanoTTS-jp のみ (README、`csrc/*.h`、
  `esp32/`、`LICENSE-MODEL.md`、Releases v1.1.0)。他の組み込み実装は参照していない。

## 1. sanoTTS-jp の要点 (公式情報から)

| 項目 | 値 / 事実 |
|---|---|
| モデル | 559 K パラメータ。Duration (33 K) → Acoustic (195 K) → iSTFT Decoder (331 K) の 3 段 |
| 重み | `saanotts-jp-v4-int8.bin` 654,032 B (blob v2、int8 conv 重みを 16 B 境界に 0 埋め)。fp32 版 2,249,792 B |
| 出力 | 22.05 kHz、float PCM。`SAAN_HOP` = 256 |
| 推論コア | `csrc/` C99、libm のみ依存、malloc 不使用 (呼び出し側が arena を渡す)。重みは mmap / flash 直参照でコピー不要 |
| ストリーミング | `saan_stream_init(st, w, arena, ids, n_ids, s_v)` → `saan_stream_pull(st, pcm, &n_out)`。8 フレーム (2048 サンプル ≈ 93 ms) 単位。受容野ぶんの遅延 36 フレーム ≈ 0.42 s。一括版とビット一致 |
| 一括合成 | `saan_synthesize()`。arena 約 1.26 MB (ストリーミング版は 176 KB) |
| RAM | arena 180,224 B、実行時 157 KB (公式 CoreS3 ファームでの値)。PSRAM 不要 (内部 SRAM 前提) |
| 速度 | CoreS3 で RTF 0.446 (W8A8 + PIE)。初回 pull 245 ms、音出しまで 407〜433 ms |
| int8 経路 | W8A32 (重み int8、活性 fp32) はプレーン C99 で fp32 とビット一致。W8A8 は PIE (ESP32-S3 SIMD) 前提で SNR 低下あり。公式は「まず W8A32、速度不足なら W8A8」 |
| G2P (かな経路) | `saan_g2p(text, nbytes, ids, cap, &n_ids, &info)`。入力はひらがな + 記号 `[` `]` `#` `_` `^` `$` + `°` (無声化) + `っ` `ん` `ー`。必要容量 `2*nbytes+3`。コード 1.5 KB、作業メモリ 0 |
| G2P (漢字経路) | 形態素解析 + アクセント推定。辞書 13.7 MB (16 MB flash 向け) / 7.1 MB / 3.0 MB。`accent.c` は形態素ノード列にアクセント規則を適用する |
| タスク | 公式ファームは合成タスク 16 KB スタック (iSTFT で 4 KB)、実測 5.4 KB 使用 |
| 重みの置き方 | 公式 DevKit 構成は `model` パーティションを `esp_partition_mmap()` (16 B 境界要件)。M5 構成は `.rodata` 埋め込み (app +643 KB) |
| ライセンス | コード MIT。重みは非 MIT (`LICENSE-MODEL.md`): 商用可、再配布可だが帰属ブロック (A) の同梱と Apache-2.0 全文 (AISHELL-3) の同梱が必須。合成音声には つくよみちゃんコーパス由来の 4 つの用途制限があり、製品の利用規約で下流ユーザーに課す義務がある |

## 2. Stack-chan 側の前提

- 発話入力は **かな** (MCP `say` はひらがな必須、jtts もかな入力)。漢字かな交じり文は扱っていない。
- jtts は `Engine { Auto, Formant, Unit, Hmm }` を持ち、`render_hmm()` のように `internal::render_*` を追加すれば新エンジンを差し込める (`components/jtts/src/jtts.cpp`)。HMM は `CONFIG_JTTS_ENABLE_HMM` で条件コンパイル。
- 再生は `main/speech.cpp` の `Speech::say()` が **16 kHz 固定** で全文を合成してから `M5.Speaker.playRaw()`。リップシンク用の包絡は同じ PCM から作る。
- ADR-001 の拡張テーブルに `sanotts` 領域 (16 MB: 0xA0000 = 655,360 B、8 MB: 同) を予約済み。
- CoreS3 の内部 RAM は HMM ロード時に逼迫する実績があり (INT free ≈ 12 KB)、176 KB の arena を内部 SRAM に取るのは現実的ではない。

## 3. 方針

### 3.1 取り込み形態

- `csrc/` を `components/saanotts/` に **ベンダリング** (MIT、hts_engine と同じ扱い)。上流の版 (v1.1.0 / コミット) を `UPSTREAM.md` に記録し、改変しない。ESP-IDF 用の CMake だけ追加。
- jtts に `Engine::Sano` と `internal::render_sano()` を追加し、`CONFIG_JTTS_ENABLE_SANOTTS` で条件コンパイル。`set_sano_weights(span)` / `sano_weights_loaded()` を HMM と同じ形で公開する。
- 公式 `esp32/` ディレクトリのファームウェアは使わない (別アプリ)。使うのは `csrc/` とビルド手順の知見だけ。

### 3.2 重みの配置と入手

- 重みは app に埋め込まず、拡張テーブルの **`sanotts` 領域に raw 配置** して `esp_partition_mmap()` で参照する (HMM 音声と同じ)。先頭に `{magic, size, crc32}` の 16 B ヘッダを置く (16 B 境界要件を保つ)。
- **領域サイズを 0xA0000 (655,360 B) から 0xB0000 (720,896 B) に広げる**ことを提案する。現行 blob 654,032 B + ヘッダで残り約 1.3 KB しかなく、blob v1→v2 で +10 KB 増えた前例がある。16 MB 機は末尾の空き 384 KiB から取れる。8 MB 機は voice を 1.25 MiB → 1.1875 MiB にする必要がある (mei 0.86 MB / nitech 1.17 MB は入る)。
- 入手経路は **機体が公式 Releases の URL から直接 HTTPS で取得** する方式を第一候補にする (`voice_fetch` と同じ仕組み)。重みを本リポジトリのリリース ZIP や Pages に同梱しないので「再配布」に当たらず、帰属ブロックと Apache-2.0 全文の同梱義務を負わない。設定ページには公式 URL と `LICENSE-MODEL.md` へのリンク、出力の用途制限 (4 項目) を表示する。
- 将来同梱する場合は `NOTICE` にブロック (A) と Apache-2.0 全文を入れ、README / 設定ページで用途制限を明示する。

### 3.3 テキスト経路 (かな → 音素 ID)

- jtts のモーラ列 (`kana_table`) から sanoTTS の **かな中間表現** (ひらがな UTF-8) を生成し、`saan_g2p()` で ID 列にする。カタカナはひらがなへ、句読点は文区切り記号へ写像する。記号の意味 (`#` `_` の扱い、文末の `$`) は `csrc/g2p.c` を読んで確定する。
- **アクセント**: かな経路にはアクセント推定が無く、無記号なら平板になる。jtts の `prosody.cpp` と同じ「句頭で上がり句末で下がる」句レベルの形だけを `[` `]` で与える (v1)。単語アクセントは辞書 (13.7 MB) が必要なので **対象外**。
- **無声化** `°`: v1 では付けない。必要なら「無声子音に挟まれた i / u」の規則を後で足す。
- 漢字経路 (形態素解析 + 辞書) は flash 予算 (16 MB 機でも 13.7 MB) と入力仕様 (かな) の両面から **対象外**。

### 3.4 合成と再生

- ストリーミング API を使う (一括版は arena 1.26 MB)。**arena 176 KB は PSRAM に確保**する (CoreS3 の内部 RAM に余裕が無い)。PSRAM 上の arena で RTF がどこまで落ちるかが最大のリスクなので、最初に実測する。W8A32 (C99 スカラー) と W8A8 + PIE の両方を測る。
- 合成は専用タスク (16 KB スタック。flash 書き込みを伴わないので PSRAM スタック可) で行う。
- **Phase 1**: ストリーミングで pull した PCM を全部集めてから既存の `Speech::say()` 経路で再生する (Speech の構造を変えない)。音出しまでの遅延は発話長 × RTF。
- **Phase 2**: pull したチャンクを逐次 `M5.Speaker.playRaw()` に流し、包絡もチャンク単位で更新する真のストリーミング再生。音出し約 0.45 s。
- **サンプルレート**: sanoTTS は 22.05 kHz。`Speech` は 16 kHz 固定なので、(a) jtts 内で 22.05 → 16 kHz にリサンプル (FIR、計算量は無視できる) するか、(b) `synthesize()` が実際のレートを返し `Speech` が再生レートと包絡ステップをそれに合わせるか。品質と単純さで **(b) を推奨** (`M5.Speaker.playRaw` はレート指定可)。

### 3.5 対象ボード

| ボード | 判断 |
|---|---|
| CoreS3 / StopWatch (16 MB + PSRAM) | 対象。`sanotts` 領域あり |
| AtomS3R (8 MB + PSRAM) | 対象候補。`sanotts` 領域あり。voice との容量配分は §3.2 |
| AtomS3 (8 MB、PSRAM なし) | 対象外。arena 176 KB を内部 SRAM に取れない |

### 3.6 検証

- ホスト: `components/saanotts/test/host` で公式の golden (`golden-v4-int8.bin`、demo ids) との一致を確認し、ベンダリングとビルドフラグの正しさを担保する。かな IR 変換は WAV 出力で試聴。
- 実機: 公式が公開している PCM checksum (v4 demo 文 `0x390bf4b2aef8f2ec`、27,136 サンプル) と一致するか。RTF、初回 pull までの時間、内部 RAM / PSRAM の使用量。

## 4. 実装ステップ

1. `components/saanotts` ベンダリング + ホスト golden テスト (CI の host-tests に追加)。
2. かな IR 変換 (`jtts` 内) + ホストで WAV 出力。
3. 実機: `sanotts` 領域の mmap、`render_sano()` (Phase 1 の一括収集)、PSRAM arena での RTF 実測。W8A32 / W8A8+PIE の比較。
4. 重み取得 (公式 URL からの device-side fetch) と設定ページの UI、用途制限の表示。
5. Phase 2 ストリーミング再生とリップシンク。
6. 句レベル アクセント記号、無声化規則。

## 5. 決定 (2026-09-15)

- 重みは機体が公式 Releases から直接取得する。本リポジトリには同梱しない (ホストテスト用にもコミットしない)。
- `sanotts` 領域は 0xB0000 (704 KiB) に広げる。8 MB 機は voice を 0x130000 (1.1875 MiB) にする。
- 22.05 kHz のまま再生する。`synthesize()` が実際のレートを返し、`Speech` が再生レートと包絡ステップを合わせる。
- ベンダリング + ホスト golden → Phase 1 (全文合成後に再生) で実機評価 → Phase 2 ストリーミング再生の順。

## 6. 実機計測 (CoreS3、2026-09-15、Phase 1)

構成: W8A8 + PIE、arena 176 KB を **PSRAM** (Quad 80 MHz)、重みは flash mmap、
データキャッシュ 32 KB。内部 RAM の空きは合成中も約 14 KB で変化なし。

| 発話 | ids | 音声長 | 合成時間 | RTF (32 B ライン) | RTF (64 B ライン) |
|---|---|---|---|---|---|
| きょ'お (先頭句) | 17 | 639 ms | 1,556 → 1,327 ms | 2.44 | 2.08 |
| わたしはすたっくちゃんです | 27 | 906 ms | 1,975 → 1,707 ms | 2.18 | 1.88 |
| (33 ids) | 33 | 1,068 ms | 2,188 ms | 2.05 | — |

- 公式の CoreS3 実測 (arena を内部 DRAM に静的確保) は RTF 0.446。PSRAM arena では
  約 4〜5 倍遅く、**RTF ≈ 2** (1 秒の音声に 2 秒)。ストリーミング再生 (Phase 2) は
  このままでは成立しない。
- データキャッシュのライン長を 64 B にすると 10〜15 % 改善 (内部 RAM を消費しない
  ので採用)。キャッシュ容量 64 KB は内部 RAM を 32 KB 食うため、Main の空き
  (約 14 KB) では不可。
- 重みの取得 (公式 Releases、654,032 B) は約 10 秒。ロード後の再生・リップシンクは
  22.05 kHz のまま動作。
- 高速化の選択肢 (未着手): (a) Main の内部 RAM を 180 KB 以上空ける (BLE / Wi-Fi /
  カメラ / ASR の常駐を見直す)、(b) 上流に「活性化バッファだけ内部 RAM に置く」
  分割 arena を提案する、(c) W8A32 で PSRAM 帯域と演算のどちらが律速か切り分ける、
  (d) Phase 1 (全文合成後に再生、文単位で分割) のまま運用する。

### 6.1 高速化の切り分け (CoreS3、2026-09-15)

| 実験 | 変更 | RTF (33 ids / 157 ids) | 判断 |
|---|---|---|---|
| 基準 | W8A8 + PIE、arena PSRAM、cache 32 KB / 64 B ライン | 1.68 / 1.54 | — |
| データキャッシュ 64 KB (カメラ無効で捻出) | +32 KB 内部 RAM 消費 | 1.64 / 1.42 | 8〜10 % しか改善せず内部 RAM が 1.5 KB まで枯渇 (ALLOC_FAIL)。不採用 |
| チャンク 16 フレーム (arena 320 KB) | 上流 `SAAN_CHUNK` を一時変更 | 1.68 / 1.48 | 重みの読み直しは律速ではない。不採用 |
| W8A32 (PIE なし) | `-DSANOTTS_ENABLE_PIE=0` | 7.6 / — | PIE の効果は約 4 倍。演算も無視できない |

結論: PSRAM 上の arena を PIE の 16 B ベクタ ロードが叩く構成では、キャッシュ容量や
チャンク長では詰まらない。RTF 0.45 に近づけるには **arena (少なくとも活性化バッファ)
を内部 DRAM に置く**しかない。カメラ無効化で内部 RAM の空きは 30 KB になったが、
176 KB には遠い。残る道は §3.4 の (a) 機能を絞ったプロファイル、(b) 上流への
分割 arena 提案、(c) Octal PSRAM ボードでの実測。

### 6.2 PSRAM が原因かの確定 (CoreS3、2026-09-15)

検証用プロファイル `cores3-sano` (会話・音声ストリーミング・ASR・カメラを外し、
IRAM 常駐コードを flash へ移し、`CONFIG_STACKCHAN_SANO_BENCH` で BLE / Wi-Fi 開始前に
固定 3 文を合成) を作り、arena を **.bss の静的配列** (公式と同じ) に置いて測った。
ヒープでは内部 RAM の最大連続ブロックが 106 KB しか無く 176 KB を確保できないため、
静的配列 (`jtts::set_sano_arena`) を使った。

| 構成 | 51 ids | 49 ids | 157 ids |
|---|---|---|---|
| 160 MHz DIO、arena PSRAM (起動直後、他タスク無し) | 1.46 | 1.41 | 1.19 |
| 160 MHz DIO、arena 内部 DRAM (静的) | 1.06 | 1.02 | 0.83 |
| 240 MHz QIO、arena 内部 DRAM (静的) | 0.68 | 0.65 | 0.53 |
| 240 MHz QIO、arena PSRAM、通常の cores3 (BLE/Wi-Fi 動作中) | 1.38 (27 ids) | — | 1.16 |

- PSRAM 上の arena は約 1.4 倍の減速要因で、主因ではない。**最大の差は CPU クロック
  (160 → 240 MHz) と flash QIO** で、これだけで約 1.5 倍速くなる。公式の 0.45 との
  残差は命令キャッシュ 32 KB / データキャッシュ 64 KB (内部 RAM 48 KB を要する) と
  推定される。
- 通常の cores3 に CPU 240 MHz + QIO を採用 (RTF 1.9 → 1.4、長文 1.5 → 1.2)。
  内部 RAM の空きは変わらず約 30 KB。
- 内部 RAM に 176 KB の連続領域を作るには、静的 DRAM (現状 209 KB、うち IRAM 常駐
  コード 120 KB) を大きく削る必要があり、Wi-Fi / BLE を持つ構成では現実的でない。
  上流への分割 arena 提案 (活性化バッファのみ内部 RAM) が残る道。

### 6.3 高速化版 (機能削減版) プロファイルの試行 (2026-09-15)

CoreS3 向けに「会話・音声ストリーミング・ASR・カメラを外し、作業領域 176 KB を .bss の
静的配列に置く」プロファイル (cores3-fast) を試したが、**現状のコードでは成立しない**。

- FreeRTOS / heap / ringbuf の関数を flash に置く設定と `SPI_FLASH_ROM_IMPL`、NimBLE
  スタック 4 KB を入れると静的 DRAM 313 KB でリンクはできるが、Wi-Fi 初期化中の NVS
  書き込みが失敗 (`ESP_ERR_WIFI_NVS`) して再起動ループになる (別の周回では
  StoreProhibited)。
- それらを外すと静的 DRAM が 8 KB 超過してリンクできない。
- ベンチ構成 (前者) で起動できた場合でも、起動直後のヒープ 99 KB では BLE / Wi-Fi /
  httpd の確保 (8 KB × 2 など) が失敗した。使える構成にするには内部 RAM がさらに
  60〜70 KB 必要で、候補は BLE を丸ごと外すこと (config_service が NimBLE を
  無条件に要求するため、それ自体が改修になる)。
- 残したもの: `CONFIG_JTTS_SANO_ARENA_STATIC` (静的配列の作業領域)、
  `CONFIG_STACKCHAN_SANO_BENCH` (起動時ベンチ)。プロファイルは削除。
- 結論: ファームウェアの分岐を避ける方針とも合わせ、通常構成 (RTF 1.4) で運用し、
  上流への分割 arena 提案を進める。
