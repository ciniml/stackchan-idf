# hts_bench — HMM ボコーダ高速化の検証ループ

実機リフラッシュを繰り返さずに MLSA ボコーダの最適化を回すためのハーネス。

## 使い方

```sh
tools/hts_bench/run.sh <voice.htsvoice> <labels.lab>
```

3 つの検証を一度に回す:

1. **正しさ (ホスト)**: components/hts_engine を x86 でビルドして WAV を生成、
   基準ハッシュ / 基準 WAV とのスペクトル差を出す。最適化が数値を壊していない
   ことを bit / スペクトルで確認。SIMD 等を入れる場合も、PC 向け intrinsic で
   同じ経路をシミュレートできる (esp32s3 の madd.s は `a*b+c` に等価)。
2. **命令スケジューリング (S3 逆アセンブル)**: xtensa-esp32s3 ツールチェインで
   -O3 -ffast-math コンパイルし、HTS_Vocoder_synthesize を objdump。madd.s の
   依存連鎖 (ストール) が減ったかを目視。FPU 命令数もカウント。
3. **サイクル計測 (実機)**: 実機の `jtts-hmm: synth N ms` ログが最終真値
   (QEMU はサイクル非正確なので不採用)。ハーネスとは別に必要時のみ 1 回焼く。

## 背景 (2026-07-08)

- gprof: `HTS_Vocoder_synthesize` が合成時間の 93.6%。48kHz で全サンプルを回し
  16kHz へ 1/3 デシメーションしている。
- ①レート削減 (48→16/24kHz、α ワープ) は**品質不成立** (フォルマント位置は
  保存されるが包絡が ~4.7dB 変形、聴取で不明瞭)。透過には本格スペクトル
  リサンプル (sp2mgc) が要り重すぎる → 不採用。
- ②MLSA 内側ループの FPU レイテンシ隠蔽: per-sample MLSA が依存連鎖で
  ストールしている (逆アセンブルで確認)。mlsadf2 が独立状態で mlsafir を
  PADEORDER 回呼ぶので、その独立チェーンをインターリーブして隠す。
- 除外: 固定小数点 SIMD (PIE) は前回検討で不成立、M系列ノイズは HTS_EMBEDDED
  で既に安価。
