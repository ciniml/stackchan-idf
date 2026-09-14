#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""ADR-001 拡張パーティションテーブル (exttab) / bootctl セクタの生成ツール。

layout JSON (exttab_16mb.json など) から 4 KiB の exttab セクタを生成する。
形式は components/flash_layout/include/flash_layout/format.h と一致させること。

  python3 tools/flash_layout/gen_exttab.py exttab_16mb.json -o build/exttab.bin
  python3 tools/flash_layout/gen_exttab.py exttab_16mb.json --print
  python3 tools/flash_layout/gen_exttab.py --bootctl main -o build/bootctl.bin

layout JSON:
  {
    "flash_size": "0x1000000",
    "reserved_offset": "0x1A0000",
    "reserved_size": "0xE60000",        # 省略時は flash_size - reserved_offset
    "generation": 1,
    "entries": [
      {"label": "main", "kind": "app", "size": "0x500000"},
      {"label": "storage", "kind": "spiffs", "size": "0x100000"},
      ...                                 # offset 省略時は前エントリの直後 (アラインメント込み)
    ]
  }
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib

SECTOR = 0x1000
APP_ALIGN = 0x10000
EXTTAB_MAGIC = 0x54584353  # 'SCXT'
EXTTAB_VERSION = 1
MAX_ENTRIES = 32
LABEL_LEN = 16
KINDS = {"app": 1, "raw": 2, "spiffs": 3, "littlefs": 4}
FLAG_REQUIRED = 0x0001

BOOTCTL_MAGIC = 0x4C434253  # 'SBCL'
BOOTCTL_VERSION = 1
TAG_LEN = 32
DEFAULT_MAX_ATTEMPTS = 3

HEADER_FMT = "<IHHIIII"  # magic, version, count, generation, reserved_offset, reserved_size, crc
ENTRY_FMT = "<II16sBBHI"  # offset, size, label, kind, format_version, flags, reserved
BOOTCTL_FMT = "<IHHIBBBB32sI"


def to_int(v) -> int:
    return int(v, 0) if isinstance(v, str) else int(v)


def crc32(data: bytes, crc: int = 0) -> int:
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def build_exttab(layout: dict) -> tuple[bytes, list[dict]]:
    flash_size = to_int(layout["flash_size"])
    reserved_offset = to_int(layout["reserved_offset"])
    reserved_size = to_int(layout.get("reserved_size", flash_size - reserved_offset))
    generation = int(layout.get("generation", 1))
    if reserved_offset % APP_ALIGN:
        raise SystemExit("reserved_offset must be 64 KiB aligned")
    if reserved_offset + reserved_size > flash_size:
        raise SystemExit("reserved range exceeds flash size")

    resolved = []
    cursor = 0
    for e in layout["entries"]:
        kind = KINDS[e["kind"]]
        align = APP_ALIGN if kind == KINDS["app"] else SECTOR
        size = to_int(e["size"])
        offset = to_int(e["offset"]) if "offset" in e else (cursor + align - 1) // align * align
        if offset % align or size % SECTOR or size == 0:
            raise SystemExit(f"{e['label']}: bad alignment (offset=0x{offset:x} size=0x{size:x})")
        if offset + size > reserved_size:
            raise SystemExit(f"{e['label']}: exceeds reserved range")
        label = e["label"].encode()
        if not label or len(label) >= LABEL_LEN:
            raise SystemExit(f"bad label {e['label']!r}")
        for r in resolved:
            if offset < r["offset"] + r["size"] and r["offset"] < offset + size:
                raise SystemExit(f"{e['label']} overlaps {r['label']}")
        flags = FLAG_REQUIRED if e.get("required", False) else 0
        resolved.append({"label": e["label"], "kind": e["kind"], "kind_id": kind, "offset": offset,
                         "size": size, "format_version": int(e.get("format_version", 1)), "flags": flags})
        cursor = offset + size
    if len(resolved) == 0 or len(resolved) > MAX_ENTRIES:
        raise SystemExit("entry count out of range")
    if not any(r["kind_id"] == KINDS["app"] for r in resolved):
        raise SystemExit("no app entry")

    entries = b"".join(
        struct.pack(ENTRY_FMT, r["offset"], r["size"], r["label"].encode().ljust(LABEL_LEN, b"\0"),
                    r["kind_id"], r["format_version"], r["flags"], 0)
        for r in resolved)
    head_wo_crc = struct.pack("<IHHIII", EXTTAB_MAGIC, EXTTAB_VERSION, len(resolved), generation,
                              reserved_offset, reserved_size)
    crc = crc32(entries, crc32(head_wo_crc))
    sector = head_wo_crc + struct.pack("<I", crc) + entries
    sector = sector.ljust(SECTOR, b"\xff")
    info = {"flash_size": flash_size, "reserved_offset": reserved_offset, "reserved_size": reserved_size,
            "generation": generation, "entries": resolved}
    return sector, info


def build_bootctl(target: str, pending: bool, tag: str, seq: int = 1,
                  max_attempts: int = DEFAULT_MAX_ATTEMPTS) -> bytes:
    t = {"recovery": 0, "main": 1}[target]
    tag_b = tag.encode()
    if len(tag_b) >= TAG_LEN:
        raise SystemExit("tag too long")
    body = struct.pack("<IHHIBBBB32s", BOOTCTL_MAGIC, BOOTCTL_VERSION, 0, seq, t, 0, max_attempts,
                       1 if pending else 0, tag_b.ljust(TAG_LEN, b"\0"))
    return (body + struct.pack("<I", crc32(body))).ljust(SECTOR, b"\xff")


def print_table(info: dict) -> None:
    ro = info["reserved_offset"]
    print(f"# flash 0x{info['flash_size']:x}, reserved 0x{ro:x} +0x{info['reserved_size']:x}, "
          f"generation {info['generation']}")
    print(f"{'label':<12} {'kind':<8} {'rel offset':>10} {'size':>9} {'abs offset':>10} {'abs end':>10}")
    for r in info["entries"]:
        print(f"{r['label']:<12} {r['kind']:<8} 0x{r['offset']:08x} 0x{r['size']:07x} "
              f"0x{ro + r['offset']:08x} 0x{ro + r['offset'] + r['size']:08x}")
    used = max(r["offset"] + r["size"] for r in info["entries"])
    print(f"# unused tail: 0x{info['reserved_size'] - used:x} bytes")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("layout", nargs="?", help="layout JSON (exttab)")
    ap.add_argument("-o", "--output", help="output .bin (1 sector). A/B は同じ内容を 2 回書く")
    ap.add_argument("--ab", action="store_true", help="A/B 2 セクタ分 (8 KiB) を出力する")
    ap.add_argument("--print", action="store_true", help="絶対オフセット付きの表を表示")
    ap.add_argument("--bootctl", choices=["main", "recovery"], help="bootctl セクタを生成する")
    ap.add_argument("--pending", action="store_true", help="bootctl: pending=1")
    ap.add_argument("--tag", default="", help="bootctl: request_tag")
    args = ap.parse_args()

    if args.bootctl:
        data = build_bootctl(args.bootctl, args.pending, args.tag)
        if args.ab:
            data = data + b"\xff" * SECTOR  # B は消去済み
    else:
        if not args.layout:
            ap.error("layout JSON required")
        with open(args.layout) as f:
            layout = json.load(f)
        data, info = build_exttab(layout)
        if args.print or not args.output:
            print_table(info)
        if args.ab:
            data = data + data
    if args.output:
        with open(args.output, "wb") as f:
            f.write(data)
        print(f"wrote {len(data)} bytes to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
