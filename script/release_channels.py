#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""リリース チャンネル (alpha / beta / rc / stable) の定義と tag の解釈。

tag は SemVer 2.0 の pre-release 形式:

    v0.15.0-alpha.1   v0.15.0-beta.2   v0.15.0-rc.1   v0.15.0

チャンネルは tag から機械的に導出する (別の入力は持たない)。順序は
alpha < beta < rc < stable で、CHANNELS の並びがそのまま「利用者がチャンネル X を
選んだとき X 以降のチャンネルが見える」という包含関係になる。
tools/settings_common.js の CHANNELS はこのミラー — 段階を足すときは両方を直す。

CI (release.yml / pages.yml) からは CLI として呼ぶ:

    release_channels.py --tag v0.15.0-alpha.1 channel   → alpha
    release_channels.py --tag v0.15.0-alpha.1 boards    → ["cores3","atoms3r"]  (JSON、matrix 用)
    release_channels.py --tag v0.15.0-alpha.1 validate  → 正しい形式なら 0、違えば 1
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from typing import Optional

# 包含順 (利用者が選ぶ「どこまで見せるか」)。stable は必ず最後。
CHANNELS: list[str] = ["alpha", "beta", "rc", "stable"]

# チャンネルごとにビルド / 配布するボード。alpha は手元で検証できる 2 ボードだけ
# (2026-09-24 決定)。beta / rc は正式版と同じ 4 ボード。
ALL_BOARDS: list[str] = ["cores3", "atoms3r", "atoms3", "stopwatch"]
BOARDS_BY_CHANNEL: dict[str, list[str]] = {
    "alpha": ["cores3", "atoms3r"],
    "beta": ALL_BOARDS,
    "rc": ALL_BOARDS,
    "stable": ALL_BOARDS,
}

_TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:-(alpha|beta|rc)\.(\d+))?$")


@dataclass(frozen=True)
class Tag:
    text: str
    major: int
    minor: int
    patch: int
    channel: str  # CHANNELS のいずれか
    number: int  # pre-release 連番 (stable は 0)

    @property
    def core(self) -> tuple[int, int, int]:
        return (self.major, self.minor, self.patch)

    @property
    def prerelease(self) -> bool:
        return self.channel != "stable"

    @property
    def sort_key(self) -> tuple[int, int, int, int, int]:
        """昇順キー。同じ X.Y.Z 内では alpha < beta < rc < stable、連番は数値比較。"""
        return (*self.core, CHANNELS.index(self.channel), self.number)


def parse_tag(text: str) -> Optional[Tag]:
    """形式に合わなければ None (旧 tag や typo は呼び出し側で扱う)。"""
    m = _TAG_RE.match(text)
    if not m:
        return None
    channel = m.group(4) or "stable"
    return Tag(text, int(m.group(1)), int(m.group(2)), int(m.group(3)), channel, int(m.group(5) or 0))


def channel_of(text: str) -> str:
    """CI 用: 形式外の tag は stable 扱い (旧 vX.Y.Z tag と同じ振る舞い)。"""
    t = parse_tag(text)
    return t.channel if t else "stable"


def visible_channels(selected: str) -> list[str]:
    """利用者が selected を選んだとき見えるチャンネル (selected 以降)。"""
    if selected not in CHANNELS:
        selected = "stable"
    return CHANNELS[CHANNELS.index(selected):]


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True)
    ap.add_argument("what", choices=["channel", "boards", "validate", "prerelease"])
    args = ap.parse_args(argv)

    tag = parse_tag(args.tag)
    if args.what == "validate":
        if tag is None:
            print(f"tag '{args.tag}' does not match vX.Y.Z[-(alpha|beta|rc).N]", file=sys.stderr)
            return 1
        return 0
    if args.what == "channel":
        print(channel_of(args.tag))
    elif args.what == "prerelease":
        print("true" if tag and tag.prerelease else "false")
    elif args.what == "boards":
        print(json.dumps(BOARDS_BY_CHANNEL[channel_of(args.tag)]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
