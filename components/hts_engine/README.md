# hts_engine (vendored)

[hts_engine API](https://hts-engine.sourceforge.net/) 1.10 の組み込み向け改変コピー。
ライセンスは Modified BSD ([COPYING](COPYING) 参照)。
Copyright (c) 2001-2015 Nagoya Institute of Technology / 2001-2008 Tokyo Institute of Technology.

## upstream (hts_engine_API-1.10) からの改変点

1. **double → float 一括変換** — ESP32-S3 の FPU は単精度のみのため。
   機械的変換: `sed 's/\bdouble\b/float/g'` + 数学関数を `expf/logf/sqrtf/powf/
   cosf/sinf/fabsf/floorf/ceilf/atanf` に置換。PDF データはもともと float32。
2. **メモリからのロード API** (`stackchan:` コメント付き)
   - `HTS_Engine_load_data()` / `HTS_ModelSet_load_data()` — flash mmap した
     .htsvoice イメージを**コピーせずに**ロードする。呼び出し側がバッファを
     ロード完了まで生かすこと (パース後は不要。木・PDF はヒープに展開される)。
   - `HTS_fopen_from_data_ref()` — 非所有ビューの HTS_File。
   - `HTS_fopen_from_fp()` の HTS_DATA 分岐をコピーからビューに変更
     (ロード時の一時ヒープ 1.4 MB 削減)。
3. **HTS_calloc が ESP_PLATFORM では PSRAM 優先** (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`、
   失敗時は内部 RAM フォールバック)。
4. portaudio 依存は `AUDIO_PLAY_NONE` で無効 (HTS_audio.c はスタブ)。

float 化の精度検証は `components/jtts/test/host` の hts ホストテスト
(double 版 CLI との出力 NRMSE 比較) を参照。
