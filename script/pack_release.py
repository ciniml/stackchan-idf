#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""ADR-001 レイアウトのリリース配布物を組み立てる。

Main のビルド (build-<board>) と Recovery のビルド (recovery/build-<board>) から、
機体に書く一式を 1 ディレクトリに集め、flash_manifest.json (書き込み位置の一覧) と
結合イメージ (0x0 から書ける 1 ファイル) を作る。

  python3 script/pack_release.py --board cores3 --version v0.13.0 --out release/cores3

出力 (ZIP に入れるもの):
  bootloader.bin        0x0       カスタム 2 段目ブートローダー (Main / Recovery で同一)
  partition-table.bin   0x8000    機体正本の標準表 (partitions_adr_<size>.csv、Recovery ビルド由来)
  bootctl.bin           0xd000    起動先 = Main (確認済み)、A/B 2 セクタ
  recovery.bin          0x10000   Recovery アプリ
  exttab.bin            0x190000  拡張パーティションテーブル A/B
  stackchan_idf.bin     0x1a0000  Main アプリ (release-fetch / BLE OTA でも使う)
  flash_manifest.json             上記の name / address (Web flasher が読む)
  firmware-<ver>-<board>.bin      結合イメージ (NVS 領域は 0xFF で埋まる = 設定は消える)
  flash_args                      esptool 用の引数ファイル

Main / Recovery のブートローダーは同じソース (bootloader_components/main) だが、
sdkconfig が違うので Main 側のものを使う (Main の sdkconfig が機体の既定)。
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# 固定領域の位置 (components/flash_layout/include/flash_layout/format.h と partitions_adr_*.csv)。
PARTS = [
    ("bootloader.bin",      0x0),
    ("partition-table.bin", 0x8000),
    ("bootctl.bin",         0xd000),
    ("recovery.bin",        0x10000),
    ("exttab.bin",          0x190000),
    ("stackchan_idf.bin",   0x1a0000),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True, help="output directory (ZIP の中身)")
    ap.add_argument("--main-build", help="default: build-<board>")
    ap.add_argument("--recovery-build", help="default: recovery/build-<board>")
    args = ap.parse_args()

    main_build = pathlib.Path(args.main_build or f"build-{args.board}")
    rcv_build = pathlib.Path(args.recovery_build or f"recovery/build-{args.board}")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    flash_size = None
    for line in (main_build / "flash_args").read_text().splitlines():
        if "--flash_size" in line:
            flash_size = line.split("--flash_size")[1].split()[0]
    if flash_size not in ("8MB", "16MB"):
        sys.exit(f"cannot determine flash size from {main_build}/flash_args")
    layout = REPO / ("exttab_16mb.json" if flash_size == "16MB" else "exttab_8mb.json")
    gen = REPO / "tools/flash_layout/gen_exttab.py"

    # 1. 生成物を集める
    shutil.copy(main_build / "bootloader/bootloader.bin", out / "bootloader.bin")
    shutil.copy(rcv_build / "partition_table/partition-table.bin", out / "partition-table.bin")
    shutil.copy(rcv_build / "stackchan_recovery.bin", out / "recovery.bin")
    shutil.copy(main_build / "stackchan_idf.bin", out / "stackchan_idf.bin")
    subprocess.check_call([sys.executable, str(gen), str(layout), "--ab", "-o", str(out / "exttab.bin")],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.check_call([sys.executable, str(gen), "--bootctl", "main", "--ab", "-o", str(out / "bootctl.bin")],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # 2. サイズ検査 (固定領域と予約範囲の境界を越えない)
    limits = {"bootloader.bin": 0x8000, "partition-table.bin": 0x1000, "bootctl.bin": 0x2000,
              "recovery.bin": 0x180000, "exttab.bin": 0x2000}
    main_limit = 0x500000 if flash_size == "16MB" else 0x400000
    limits["stackchan_idf.bin"] = main_limit
    for name, _ in PARTS:
        size = (out / name).stat().st_size
        if size > limits[name]:
            sys.exit(f"{name} is {size} B, exceeds {limits[name]} B")

    # 3. マニフェストと flash_args
    manifest = {
        "layout": "adr-001",
        "board": args.board,
        "version": args.version,
        "flash_size": flash_size,
        "parts": [{"name": n, "address": f"0x{a:x}", "size": (out / n).stat().st_size} for n, a in PARTS],
    }
    (out / "flash_manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    with open(out / "flash_args", "w") as f:
        f.write(f"--flash_mode dio --flash_freq 80m --flash_size {flash_size}\n")
        for n, a in PARTS:
            f.write(f"0x{a:x} {n}\n")

    # 4. 結合イメージ (0x0 から 1 ファイルで書ける。隙間は 0xFF)
    combined = out / f"firmware-{args.version}-{args.board}.bin"
    with open(combined, "wb") as f:
        cur = 0
        for n, a in PARTS:
            if cur < a:
                f.write(b"\xff" * (a - cur))
                cur = a
            data = (out / n).read_bytes()
            f.write(data)
            cur += len(data)
    print(f"packed {args.board} {args.version} ({flash_size}) into {out}: combined {combined.stat().st_size} B")
    for p in manifest["parts"]:
        print(f"  {p['address']:>9}  {p['name']:<20} {p['size']:>8} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
