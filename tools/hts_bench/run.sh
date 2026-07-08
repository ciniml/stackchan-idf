#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# HMM ボコーダ高速化の検証ループ (README.md 参照)。
#   run.sh <voice.htsvoice> <labels.lab> [ref_dir]
# 正しさ (ホスト WAV/ハッシュ) + 命令スケジューリング (S3 逆アセンブル) を回す。
set -euo pipefail

VOICE="${1:?usage: run.sh <voice.htsvoice> <labels.lab> [ref_dir]}"
LABELS="${2:?labels.lab required}"
REF="${3:-/tmp/hts_bench_ref}"
HTS="$(cd "$(dirname "$0")/../../components/hts_engine" && pwd)"
WORK="/tmp/hts_bench_work"
mkdir -p "$WORK" "$REF"

# --- toolchains ---
S3GCC="$(ls "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc 2>/dev/null | sort | tail -1)"
S3DUMP="$(ls "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump 2>/dev/null | sort | tail -1)"

# --- 1. host correctness: build + synth WAV ---
cat > "$WORK/synth.c" <<'EOF'
#include <stdio.h>
#include "HTS_engine.h"
int main(int c,char**v){HTS_Engine e;HTS_Engine_initialize(&e);char*vv[1]={v[1]};
 if(!HTS_Engine_load(&e,vv,1))return 1;
 if(!HTS_Engine_synthesize_from_fn(&e,v[2]))return 1;
 FILE*f=fopen(v[3],"wb");size_t n=HTS_Engine_get_nsamples(&e);
 for(size_t i=0;i<n;i++){float x=HTS_Engine_get_generated_speech(&e,i);
  short s=x>32767?32767:x<-32768?-32768:(short)x;fwrite(&s,2,1,f);}
 fclose(f);fprintf(stderr,"nsamples=%zu rate=%zu\n",n,HTS_Engine_get_sampling_frequency(&e));return 0;}
EOF
cc -O2 -DAUDIO_PLAY_NONE -DHTS_EMBEDDED -I"$HTS/lib" -I"$HTS/include" \
   "$WORK/synth.c" "$HTS"/lib/*.c -lm -o "$WORK/synth"
"$WORK/synth" "$VOICE" "$LABELS" "$WORK/out.raw"
HASH=$(sha256sum "$WORK/out.raw" | cut -c1-16)
echo "[host] out.raw sha256=$HASH"
if [ -f "$REF/out.raw" ]; then
  if cmp -s "$WORK/out.raw" "$REF/out.raw"; then
    echo "[host] ✓ bit-identical to reference"
  else
    python3 - "$WORK/out.raw" "$REF/out.raw" <<'PY'
import sys,numpy as np
a=np.fromfile(sys.argv[1],dtype=np.int16).astype(float);b=np.fromfile(sys.argv[2],dtype=np.int16).astype(float)
n=min(len(a),len(b));a,b=a[:n],b[:n]
d=a-b;snr=10*np.log10((b**2).mean()/max((d**2).mean(),1e-9))
print(f"[host] ≠ reference: max|Δ|={int(abs(d).max())} SNR={snr:.1f}dB (位相ドリフトなら高SNR)")
PY
  fi
else
  cp "$WORK/out.raw" "$REF/out.raw"; echo "[host] reference saved (first run)"
fi

# --- 2. S3 disassembly: FPU scheduling of the vocoder ---
if [ -n "$S3GCC" ] && [ -n "$S3DUMP" ]; then
  "$S3GCC" -c -O3 -ffast-math -fno-strict-aliasing -mlongcalls \
    -DAUDIO_PLAY_NONE -DHTS_EMBEDDED -DESP_PLATFORM \
    -I"$HTS/lib" -I"$HTS/include" "$HTS/lib/HTS_vocoder.c" -o "$WORK/vocoder.o"
  echo "[s3] FPU 命令数 (HTS_vocoder.o):"
  "$S3DUMP" -d "$WORK/vocoder.o" | grep -oE '\b(madd\.s|msub\.s|mul\.s|add\.s|sub\.s)\b' \
    | sort | uniq -c | sort -rn | sed 's/^/[s3]   /'
  "$S3DUMP" -d "$WORK/vocoder.o" | awk '/<HTS_Vocoder_synthesize>:/{f=1} f' > "$WORK/vsynth.s"
  echo "[s3] 逆アセンブル: $WORK/vsynth.s (HTS_Vocoder_synthesize 全体)"
else
  echo "[s3] xtensa-esp32s3 toolchain 未検出 — S3 逆アセンブルはスキップ"
fi
echo "done. 実機タイミングは別途 'jtts-hmm: synth N ms' ログで確認。"
