#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""
Saleae Logic 2 の I2C analyzer CSV を読んで、生のイベント列
(start/address/data/stop) を「論理的なレジスタ アクセス 1 件 = 1 行」に
たたみ込むツール。

Logic 2 の CSV は START / Repeated-START / ADDRESS(R/W) / DATA / STOP の
時系列イベント。1 つの STOP までを 1 トランザクションとし、その中の
セグメント (1 つの START から次の START or STOP まで) を解析して

  ・WRITE のみ (1 segment, addr=W, data ≥ 1 byte)
      → register pointer + 値 0..N の書込
  ・WRITE + Repeated-START + READ (2 segments)
      → register pointer 設定 → 連続読出 (auto-increment)
  ・ADDRESS のみ (data 0 byte)
      → probe / ping

を判定して 1 行出力する。

Usage:
    python tools/i2c_decode.py i2c.csv
    python tools/i2c_decode.py i2c.csv --addr 0x34          # 特定アドレスのみ
    python tools/i2c_decode.py i2c.csv --from 1.0 --to 2.0  # 時間範囲フィルタ
    python tools/i2c_decode.py i2c.csv --no-names           # チップ名付与 OFF
"""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass, field

# 内蔵 デバイス名表。stackchan-idf の対象ボード上で見える代表的なアドレスのみ。
# 不明アドレスは「?」表示。
KNOWN_DEVICES: dict[int, str] = {
    0x34: "AXP2101",
    0x38: "FT6336U",  # CoreS3 touch (alt 0x48)
    0x40: "INA226",   # M5Base battery monitor
    0x48: "FT6336U",
    0x51: "BM8563",   # CoreS3 RTC
    0x58: "AW9523",   # CoreS3 IO expander
    0x68: "BMI270",   # CoreS3 IMU
    0x6F: "PY32",     # Stack-chan base IO expander
}


@dataclass
class Segment:
    """1 つの START からその次の START / STOP までのデータ。"""
    addr: int | None = None
    read: bool = False          # ADDRESS の R/W bit
    addr_acked: bool = False
    data_bytes: list[int] = field(default_factory=list)

    def __str__(self) -> str:
        op = "R" if self.read else "W"
        ack = "ACK" if self.addr_acked else "NAK"
        return f"{op} {self.addr:#04x} [{ack}] {[hex(b) for b in self.data_bytes]}"


@dataclass
class Transaction:
    """START から STOP までの 1 トランザクション。"""
    start_time: float
    segments: list[Segment] = field(default_factory=list)


def parse_csv(path: str) -> list[Transaction]:
    """Logic 2 CSV を Transaction のリストに変換。"""
    transactions: list[Transaction] = []
    cur_tx: Transaction | None = None
    cur_seg: Segment | None = None

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ev = row["type"]
            t = float(row["start_time"])
            if ev == "start":
                if cur_tx is None:
                    # 通常 start = 新規トランザクション
                    cur_tx = Transaction(start_time=t)
                else:
                    # Repeated-START (前の STOP 無しで再 start) は同じ tx の
                    # 新セグメント。前 segment を tx に押し込む。
                    if cur_seg is not None:
                        cur_tx.segments.append(cur_seg)
                cur_seg = Segment()
            elif ev == "address":
                if cur_seg is None:
                    # START 無しで ADDRESS だけ来るのは普通 ないが、防御的に。
                    cur_seg = Segment()
                cur_seg.addr = int(row["address"], 0)
                cur_seg.read = row["read"].lower() == "true"
                cur_seg.addr_acked = row["ack"].lower() == "true"
            elif ev == "data":
                if cur_seg is None:
                    continue
                cur_seg.data_bytes.append(int(row["data"], 0))
            elif ev == "stop":
                if cur_seg is not None and cur_tx is not None:
                    cur_tx.segments.append(cur_seg)
                if cur_tx is not None:
                    transactions.append(cur_tx)
                cur_tx = None
                cur_seg = None
    # 末尾に stop が来てないトランザクションも記録 (capture 終端切れ等)。
    if cur_tx is not None:
        if cur_seg is not None:
            cur_tx.segments.append(cur_seg)
        transactions.append(cur_tx)
    return transactions


def device_name(addr: int | None) -> str:
    """アドレス → "0x34=AXP2101" 形式の文字列。不明は "0x34=?"。"""
    if addr is None:
        return "??"
    name = KNOWN_DEVICES.get(addr, "?")
    return f"{addr:#04x}={name}"


def classify(tx: Transaction) -> tuple[str, int | None, int | None, list[int], list[Segment]]:
    """Transaction を (op, addr, reg, data_bytes, raw_segments) に変換。

    op:
      READ     : seg=2 (addr=W,1byte) + (addr=R,N byte) — register read
      WRITE    : seg=1 (addr=W, ≥1 byte) — register pointer + 0..N byte 書込
                 (1 byte の場合は「register pointer のみ」だが、表示上は
                 WRITE 0 byte 扱い、最後の byte を data に)
      READ_RAW : seg=1 (addr=R) — 起動直後等、いきなり read
                 (内部ポインタが既に設定されてる前提の chips がある)
      PROBE    : addr=W, data 0 byte
      OTHER    : 上記いずれも該当しない (3 segments 以上、エラー等)
    """
    segs = tx.segments
    if len(segs) == 2:
        s1, s2 = segs
        if (not s1.read and s2.read
                and len(s1.data_bytes) == 1):
            return ("READ", s2.addr, s1.data_bytes[0], s2.data_bytes, segs)
    if len(segs) == 1:
        s = segs[0]
        if not s.read:
            if len(s.data_bytes) == 0:
                return ("PROBE", s.addr, None, [], segs)
            elif len(s.data_bytes) == 1:
                # ambiguous: register pointer set OR 1 byte write to reg 0.
                # 慣習的に「register pointer 設定」扱い、data を reg として
                # 出力 (data は無し)。実 chip 動作と合わない場合は呼出側で
                # 解釈し直してもらう。
                return ("WRITE_REG", s.addr, s.data_bytes[0], [], segs)
            else:
                return ("WRITE", s.addr, s.data_bytes[0], s.data_bytes[1:], segs)
        else:
            return ("READ_RAW", s.addr, None, s.data_bytes, segs)
    return ("OTHER", None, None, [], segs)


def fmt_bytes(bs: list[int], max_show: int = 16) -> str:
    if not bs:
        return ""
    if len(bs) <= max_show:
        return " ".join(f"{b:02X}" for b in bs)
    head = " ".join(f"{b:02X}" for b in bs[:max_show])
    return f"{head} ... ({len(bs)} bytes)"


def format_line(tx: Transaction, with_names: bool) -> str:
    op, addr, reg, data_bytes, segs = classify(tx)
    dev = device_name(addr) if with_names else (f"{addr:#04x}" if addr is not None else "??")
    if op == "READ":
        return f"{tx.start_time:>12.6f}  R   {dev:<14}  reg={reg:#04x}  → {fmt_bytes(data_bytes)}"
    if op == "WRITE":
        return f"{tx.start_time:>12.6f}  W   {dev:<14}  reg={reg:#04x}  ← {fmt_bytes(data_bytes)}"
    if op == "WRITE_REG":
        # pointer-only set, 値を書いてない。診断上 reg だけ書いた旨を残す。
        return f"{tx.start_time:>12.6f}  W   {dev:<14}  reg={reg:#04x}  (ptr only)"
    if op == "READ_RAW":
        return f"{tx.start_time:>12.6f}  R*  {dev:<14}  (no reg ptr) → {fmt_bytes(data_bytes)}"
    if op == "PROBE":
        return f"{tx.start_time:>12.6f}  P   {dev:<14}  (address probe)"
    # OTHER — raw segments を全部出す
    body = " | ".join(str(s) for s in segs)
    return f"{tx.start_time:>12.6f}  ?   {body}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="Saleae Logic 2 I2C analyzer CSV (start/address/data/stop)")
    ap.add_argument("--addr", action="append", default=[],
                    help="特定 I2C アドレスのみ表示 (例: 0x34、--addr を複数指定可)")
    ap.add_argument("--from", dest="t_from", type=float, default=None,
                    help="この時刻 (秒) 以降のみ表示")
    ap.add_argument("--to", dest="t_to", type=float, default=None,
                    help="この時刻 (秒) 以前のみ表示")
    ap.add_argument("--no-names", action="store_true",
                    help="既知デバイス名 (AXP2101 等) の付与をやめる")
    ap.add_argument("--show-other", action="store_true",
                    help="OTHER (= 解釈失敗) も含めて表示 (デフォルトは表示する)")
    ap.add_argument("--only-writes", action="store_true",
                    help="WRITE / WRITE_REG のみ表示 (誤書込の追跡用)")
    ap.add_argument("--only-reads", action="store_true",
                    help="READ / READ_RAW のみ表示")
    args = ap.parse_args()

    filter_addrs: set[int] | None = None
    if args.addr:
        try:
            filter_addrs = {int(a, 0) for a in args.addr}
        except ValueError as e:
            print(f"error: invalid --addr value: {e}", file=sys.stderr)
            return 1

    transactions = parse_csv(args.csv)
    n_total = len(transactions)
    n_shown = 0
    n_dropped_other = 0

    print(f"#       time   op   device          info")
    print(f"# ──────────  ──   ──────────────  ─────────────────────────────────────")
    for tx in transactions:
        if args.t_from is not None and tx.start_time < args.t_from:
            continue
        if args.t_to is not None and tx.start_time > args.t_to:
            continue
        op, addr, _reg, _data, _segs = classify(tx)
        if filter_addrs is not None and addr not in filter_addrs:
            continue
        if op == "OTHER" and not args.show_other:
            n_dropped_other += 1
            continue
        if args.only_writes and op not in ("WRITE", "WRITE_REG"):
            continue
        if args.only_reads and op not in ("READ", "READ_RAW"):
            continue
        print(format_line(tx, with_names=not args.no_names))
        n_shown += 1

    # サマリ
    print()
    print(f"# total transactions: {n_total}, shown: {n_shown}", end="")
    if n_dropped_other:
        print(f", dropped 'OTHER' (use --show-other to include): {n_dropped_other}", end="")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
