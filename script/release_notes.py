#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""GitHub Release の本文 (Markdown) を組み立てる。

方針 (2026-09-24):
  - 本文の中心は **そのリリースの主要変更点**。固定の機能一覧は載せない (README にある)。
  - 変更点は docs/releases/<tag>.md (手書き、利用者向けの言葉で) があればそれを使い、
    無ければ直前の tag からのコミット一覧で代用する (merge / ci / chore / build は除く)。
    直前の tag = stable なら直前の stable、pre-release なら (チャンネル問わず) 直前の tag。
  - ADR 番号や内部の設計用語は書かない (利用者は知らない)。
  - 恒常的な案内 (対応ボード / 書き込み方法 / OTA / v0.12 以前からの更新) は短く末尾に。

使い方 (release.yml の release ジョブから):
    release_notes.py --tag v0.15.0-alpha.1 --boards '["cores3","atoms3r"]' \\
        --repo ciniml/stackchan-idf --commit <sha> --build-date "..." > release_notes.md
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_channels import Tag, parse_tag  # noqa: E402

BOARD_LABELS = {
    "cores3": "**CoreS3 + M5/Takao ベース** (`firmware-{v}-cores3.zip`、16 MB flash / Quad PSRAM)",
    "atoms3r": "**AtomS3R + Atomic ECHO BASE** \"アトムニャン\" (`firmware-{v}-atoms3r.zip`、8 MB flash / Octal PSRAM)",
    "atoms3": "**AtomS3 + Atomic ECHO BASE** slim プロファイル (`firmware-{v}-atoms3.zip`、8 MB flash / PSRAM なし)",
    "stopwatch": "**M5 StopWatch (C152)** 円形 AMOLED 466×466 + ES8311 + BMI270 + RX8130CE + 振動モータ "
                 "(`firmware-{v}-stopwatch.zip`、16 MB flash / Octal PSRAM)",
}

# 自動生成の変更点から落とすコミット (利用者に意味が無いもの)。
_SKIP_SUBJECT = re.compile(r"^(Merge |ci\(|ci:|chore|build\(|build:|docs\(|docs:|test\(|test:)")


def previous_tag(tag: Tag, all_tags: list[str]) -> Optional[str]:
    """変更点の起点。stable なら直前の stable、pre-release なら直前の tag (チャンネル問わず)。"""
    parsed = [t for t in (parse_tag(s) for s in all_tags) if t and t.sort_key < tag.sort_key]
    if not tag.prerelease:
        parsed = [t for t in parsed if not t.prerelease]
    if not parsed:
        return None
    return max(parsed, key=lambda t: t.sort_key).text


def git_tags(repo_dir: Path) -> list[str]:
    out = subprocess.run(["git", "tag", "--list", "v*"], cwd=repo_dir, capture_output=True, text=True, check=True)
    return out.stdout.split()


def auto_changes(repo_dir: Path, prev: Optional[str], tag: str) -> list[str]:
    rng = f"{prev}..{tag}" if prev else tag
    out = subprocess.run(["git", "log", "--no-merges", "--format=%s", rng], cwd=repo_dir,
                         capture_output=True, text=True, check=True)
    return [s for s in out.stdout.splitlines() if s.strip() and not _SKIP_SUBJECT.match(s)]


def hand_written(repo_dir: Path, tag: str) -> Optional[str]:
    p = repo_dir / "docs" / "releases" / f"{tag}.md"
    return p.read_text(encoding="utf-8").strip() if p.exists() else None


def build_body(tag_text: str, boards: list[str], repo: str, commit: str, build_date: str, idf: str,
               repo_dir: Path) -> str:
    tag = parse_tag(tag_text)
    if tag is None:
        raise SystemExit(f"bad tag {tag_text}")
    owner, name = repo.split("/", 1)
    pages = f"https://{owner}.github.io/{name}"
    lines: list[str] = [f"# Stack-chan Firmware {tag_text}", ""]

    if tag.prerelease:
        lines += [
            f"> **⚠ {tag.channel} 版 (pre-release)** — 検証用のビルドです。対応ボードは下記のとおりで、"
            f"正式版は [Latest](https://github.com/{repo}/releases/latest) を参照してください。"
            f"Web Flasher / 設定ページでは「チャンネル」で **{tag.channel}** を選ぶと表示されます。",
            "",
        ]

    prev = previous_tag(tag, git_tags(repo_dir))
    notes = hand_written(repo_dir, tag_text)
    lines += ["## 変更点" + (f" ({prev} から)" if prev else ""), ""]
    if notes:
        lines += [notes, ""]
    else:
        changes = auto_changes(repo_dir, prev, tag_text)
        lines += [f"- {c}" for c in changes] or ["- (変更点の記載なし)"]
        lines += [""]
    if prev:
        lines += [f"すべてのコミット: https://github.com/{repo}/compare/{prev}...{tag_text}", ""]

    lines += ["## 対応ボード (このリリースに含まれる firmware)", ""]
    lines += [f"- {BOARD_LABELS[b].format(v=tag_text)}" for b in boards]
    lines += ["", "PSRAM モードは bootloader レベルで確定するため、ボードごとに別 firmware です。", ""]

    lines += [
        "## インストール",
        "",
        "### Web Flasher (Chrome / Edge)",
        f"<{pages}/> でこのリリースを選び、ボードを選んでから書き込んでください"
        + (f" (「Channel」で **{tag.channel}** を選ぶか `?channel={tag.channel}` を付けて開く)。" if tag.prerelease else "。"),
        "",
        "### OTA (v0.13.0 以降が動いている場合)",
        f"<{pages}/settings.html> に BLE 接続し「ファームウェア更新 (OTA)」でこのリリースを選ぶと、"
        "機体が Wi-Fi で取得して再起動します (約 30 秒)。Wi-Fi が無い場合は同梱の `stackchan_idf.bin` を"
        "選ぶと BLE で転送します (4〜5 分)。",
        "",
        "### コマンドライン (esptool)",
        "ボードに合った ZIP を展開し、結合イメージを 0x0 に書き込みます (設定も初期化されます):",
        "```bash",
        f"esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash 0x0 firmware-{tag_text}-<board>.bin",
        "```",
        "設定を残したい場合は `flash_args` の一覧どおりに個別に書き込みます: `write_flash @flash_args`",
        "",
        "> **v0.12 以前から更新する場合**: v0.13.0 で flash の構成が変わったため、BLE / Wi-Fi の OTA では"
        "移行できません。一度 USB (Web Flasher または esptool) で書き込んでください (保存済みの顔データ・"
        "HMM ボイス・sanoTTS の重みは消えます。Wi-Fi などの設定は残ります)。詳細は "
        f"[v0.13.0 のリリースノート](https://github.com/{repo}/releases/tag/v0.13.0)。",
        "",
        f"機能の一覧は [README](https://github.com/{repo}#readme)、第三者ライセンス / 帰属表示は "
        f"<{pages}/licenses.html>。",
        "",
        "## ビルド情報",
        f"- ESP-IDF {idf} / ESP32-S3",
        f"- Build: {build_date}",
        f"- Commit: {commit}",
    ]
    return "\n".join(lines) + "\n"


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--boards", required=True, help="JSON 配列")
    ap.add_argument("--repo", default="ciniml/stackchan-idf")
    ap.add_argument("--commit", default="")
    ap.add_argument("--build-date", default="")
    ap.add_argument("--idf", default="v5.5.5")
    ap.add_argument("--repo-dir", type=Path, default=Path(__file__).resolve().parent.parent)
    a = ap.parse_args(argv)
    sys.stdout.write(build_body(a.tag, json.loads(a.boards), a.repo, a.commit, a.build_date, a.idf, a.repo_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
