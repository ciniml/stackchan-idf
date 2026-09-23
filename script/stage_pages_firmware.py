#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""GitHub Release の firmware ZIP を Pages サイトへミラーし、versions.json を作る。

pages.yml から呼ばれる (以前は bash + jq のループだった)。出力:

    <site>/firmware/<tag>/firmware-<tag>-<board>.zip        Web Flasher / BLE OTA が読む ZIP
    <site>/firmware/<tag>/<board>/stackchan_idf.bin         機体の release-fetch OTA が読む app 単体
    <site>/versions.json         stable だけ (配布済み機体の /api/release/versions が読む。
                                 先頭 = 最新として既定選択されるので、ここに pre-release は混ぜない)
    <site>/versions-all.json     全チャンネル + channel / prerelease / published_at

エントリの形 (両ファイル共通):
    {"tag": "v0.15.0-alpha.2", "channel": "alpha", "prerelease": true,
     "published_at": "2026-09-25T03:00:00Z",
     "boards": {"cores3": "firmware-v0.15.0-alpha.2-cores3.zip", ...}}

保持ルール:
    - stable: RETAIN_FLOOR 以上を全部 (Pages の 1 GB 上限対策。1 リリース ≈ 25 MB)
    - pre-release: 「同じ X.Y.Z 以上の stable がまだ無い」ものだけ (進行中のもの)。
      v0.15.0 を出したら v0.15.0-alpha.* / -beta.* / -rc.* は Pages から消える
      (GitHub Releases 上には残る)
並び順は両ファイルとも新しい版が先 (semver 順。作成日順ではない)。

`gh` の呼び出しは subprocess。テスト (test_stage_pages_firmware.py) は GhClient を差し替える。
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_channels import ALL_BOARDS, Tag, parse_tag  # noqa: E402

RETAIN_FLOOR = "v0.7.0"


@dataclass
class Release:
    tag: str
    published_at: str
    is_prerelease: bool  # GitHub 側のフラグ (参考。真実は tag の形式)
    assets: list[str] = field(default_factory=list)  # firmware-*.zip の名前


class GhClient:
    """gh CLI の薄いラッパ。テストでは同じメソッドを持つ偽物に差し替える。"""

    def __init__(self, repo: str):
        self.repo = repo

    def _run(self, *args: str) -> str:
        return subprocess.run(["gh", *args, "--repo", self.repo], check=True, capture_output=True, text=True).stdout

    def list_releases(self) -> list[Release]:
        out = self._run("release", "list", "--limit", "100", "--json", "tagName,isDraft,isPrerelease,publishedAt")
        rels = []
        for r in json.loads(out):
            if r.get("isDraft"):
                continue
            rels.append(Release(r["tagName"], r.get("publishedAt") or "", bool(r.get("isPrerelease"))))
        return rels

    def list_assets(self, tag: str) -> list[str]:
        out = self._run("release", "view", tag, "--json", "assets")
        names = [a["name"] for a in json.loads(out).get("assets", [])]
        return [n for n in names if n.startswith("firmware-") and n.endswith(".zip")]

    def download(self, tag: str, asset: str, dest: Path) -> None:
        dest.mkdir(parents=True, exist_ok=True)
        self._run("release", "download", tag, "-p", asset, "-D", str(dest), "--clobber")


def board_of_asset(asset: str) -> str:
    """firmware-vX.Y.Z-<board>.zip → board。接尾辞が無い旧 ZIP は cores3 (単一 ZIP 時代)。"""
    base = asset[: -len(".zip")]
    suffix = base.rsplit("-", 1)[-1]
    return suffix if suffix in ALL_BOARDS else "cores3"


def select_releases(releases: list[Release], floor: str = RETAIN_FLOOR) -> list[tuple[Tag, Release]]:
    """保持ルールを適用し、新しい順に並べて返す。形式外の tag はログして落とす。"""
    floor_tag = parse_tag(floor)
    assert floor_tag is not None, floor
    parsed: list[tuple[Tag, Release]] = []
    for r in releases:
        t = parse_tag(r.tag)
        if t is None:
            print(f"[skip] {r.tag}: not vX.Y.Z[-(alpha|beta|rc).N]", file=sys.stderr)
            continue
        if t.prerelease != r.is_prerelease:
            print(f"[warn] {r.tag}: GitHub prerelease flag = {r.is_prerelease} but tag says {t.channel}",
                  file=sys.stderr)
        parsed.append((t, r))

    stable_cores = [t.core for t, _ in parsed if not t.prerelease]
    kept: list[tuple[Tag, Release]] = []
    for t, r in parsed:
        if not t.prerelease:
            if t.sort_key < floor_tag.sort_key:
                print(f"[skip] {r.tag}: below retention floor {floor}", file=sys.stderr)
                continue
        elif any(c >= t.core for c in stable_cores):
            print(f"[skip] {r.tag}: superseded by a stable release", file=sys.stderr)
            continue
        kept.append((t, r))
    kept.sort(key=lambda tr: tr[0].sort_key, reverse=True)
    return kept


def stage(gh: GhClient, site: Path, floor: str = RETAIN_FLOOR, download: bool = True) -> list[dict]:
    """ZIP を site/firmware/ に置き、versions.json / versions-all.json を書く。全エントリを返す。"""
    fw_root = site / "firmware"
    fw_root.mkdir(parents=True, exist_ok=True)
    entries: list[dict] = []
    for t, r in select_releases(gh.list_releases(), floor):
        assets = gh.list_assets(r.tag)
        if not assets:
            print(f"[skip] {r.tag}: no firmware ZIP asset", file=sys.stderr)
            continue
        dest = fw_root / r.tag
        boards: dict[str, str] = {}
        for asset in assets:
            board = board_of_asset(asset)
            boards[board] = asset
            if not download:
                continue
            gh.download(r.tag, asset, dest)
            # OTA 用に app イメージだけ取り出す (結合イメージ firmware-<tag>-<board>.bin ではない)。
            # 無い ZIP (release-fetch 対応前) は黙って飛ばす。
            try:
                with zipfile.ZipFile(dest / asset) as z:
                    data = z.read("stackchan_idf.bin")
            except (KeyError, zipfile.BadZipFile):
                data = b""
            if data:
                (dest / board).mkdir(parents=True, exist_ok=True)
                (dest / board / "stackchan_idf.bin").write_bytes(data)
                print(f"[ok] {r.tag}/{board}/stackchan_idf.bin ({len(data)} bytes)")
            print(f"[ok] {r.tag}/{board} -> {dest / asset}")
        entries.append({
            "tag": r.tag,
            "channel": t.channel,
            "prerelease": t.prerelease,
            "published_at": r.published_at,
            "boards": boards,
        })

    stable = [e for e in entries if not e["prerelease"]]
    (site / "versions.json").write_text(json.dumps(stable, indent=2, ensure_ascii=False) + "\n")
    (site / "versions-all.json").write_text(json.dumps(entries, indent=2, ensure_ascii=False) + "\n")
    return entries


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "ciniml/stackchan-idf"))
    ap.add_argument("--site", type=Path, default=Path("_site"))
    ap.add_argument("--floor", default=RETAIN_FLOOR)
    ap.add_argument("--no-download", action="store_true", help="versions*.json だけ作る (動作確認用)")
    args = ap.parse_args(argv)

    entries = stage(GhClient(args.repo), args.site, args.floor, download=not args.no_download)
    print("--- versions-all.json ---")
    for e in entries:
        print(f"  {e['tag']:<20} {e['channel']:<7} {' '.join(sorted(e['boards']))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
