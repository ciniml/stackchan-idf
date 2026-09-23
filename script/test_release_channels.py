#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""python3 script/test_release_channels.py — release_channels.py と stage_pages_firmware.py の単体テスト。"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_channels as rc  # noqa: E402
import stage_pages_firmware as sp  # noqa: E402


class ParseTagTest(unittest.TestCase):
    def test_stable(self):
        t = rc.parse_tag("v0.14.3")
        self.assertEqual((t.core, t.channel, t.number, t.prerelease), ((0, 14, 3), "stable", 0, False))

    def test_prerelease(self):
        t = rc.parse_tag("v0.15.0-alpha.10")
        self.assertEqual((t.core, t.channel, t.number, t.prerelease), ((0, 15, 0), "alpha", 10, True))

    def test_rejects(self):
        for bad in ["0.15.0", "v0.15", "v0.15.0-alpha1", "v0.15.0-alpha", "v0.15.0-nightly.1", "v0.15.0-dirty", "V0.15.0"]:
            self.assertIsNone(rc.parse_tag(bad), bad)

    def test_order(self):
        tags = ["v0.15.0", "v0.15.0-alpha.1", "v0.15.0-beta.2", "v0.14.4", "v0.15.0-rc.1", "v0.15.0-alpha.10", "v0.15.1"]
        got = sorted(tags, key=lambda s: rc.parse_tag(s).sort_key)
        self.assertEqual(got, ["v0.14.4", "v0.15.0-alpha.1", "v0.15.0-alpha.10", "v0.15.0-beta.2",
                               "v0.15.0-rc.1", "v0.15.0", "v0.15.1"])

    def test_visible_channels(self):
        self.assertEqual(rc.visible_channels("stable"), ["stable"])
        self.assertEqual(rc.visible_channels("beta"), ["beta", "rc", "stable"])
        self.assertEqual(rc.visible_channels("bogus"), ["stable"])

    def test_boards(self):
        self.assertEqual(rc.BOARDS_BY_CHANNEL["alpha"], ["cores3", "atoms3r"])
        self.assertEqual(rc.BOARDS_BY_CHANNEL["stable"], rc.ALL_BOARDS)
        self.assertEqual(rc.CHANNELS[-1], "stable")

    def test_cli(self):
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(rc.main(["--tag", "v0.15.0-alpha.1", "boards"]), 0)
            self.assertEqual(rc.main(["--tag", "v0.15.0-alpha.1", "channel"]), 0)
            self.assertEqual(rc.main(["--tag", "v0.15.0", "prerelease"]), 0)
        self.assertEqual(out.getvalue().split("\n")[:3], ['["cores3", "atoms3r"]', "alpha", "false"])
        self.assertEqual(rc.main(["--tag", "v0.15.0", "validate"]), 0)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(rc.main(["--tag", "v0.15.0-alpha1", "validate"]), 1)


class FakeGh:
    def __init__(self, releases: dict[str, dict]):
        # tag → {"prerelease": bool, "assets": [names], "published_at": str}
        self.releases = releases
        self.downloaded: list[tuple[str, str]] = []

    def list_releases(self):
        return [sp.Release(tag, r.get("published_at", "2026-01-01T00:00:00Z"), r["prerelease"])
                for tag, r in self.releases.items()]

    def list_assets(self, tag):
        return list(self.releases[tag]["assets"])

    def download(self, tag, asset, dest: Path):
        self.downloaded.append((tag, asset))
        dest.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(dest / asset, "w") as z:
            z.writestr("stackchan_idf.bin", f"{tag}:{asset}".encode())


def _rel(prerelease: bool, *boards: str, tag: str = "") -> dict:
    return {"prerelease": prerelease, "assets": [f"firmware-{tag}-{b}.zip" for b in boards]}


class SelectReleasesTest(unittest.TestCase):
    def _sel(self, tags: dict[str, bool], floor="v0.7.0"):
        rels = [sp.Release(t, "", p) for t, p in tags.items()]
        return [r.tag for _, r in sp.select_releases(rels, floor)]

    def test_floor_and_order(self):
        got = self._sel({"v0.6.9": False, "v0.7.0": False, "v0.14.3": False, "v0.14.2": False})
        self.assertEqual(got, ["v0.14.3", "v0.14.2", "v0.7.0"])

    def test_inflight_prerelease_kept(self):
        got = self._sel({"v0.14.3": False, "v0.15.0-alpha.1": True, "v0.15.0-alpha.2": True})
        self.assertEqual(got, ["v0.15.0-alpha.2", "v0.15.0-alpha.1", "v0.14.3"])

    def test_superseded_prerelease_dropped(self):
        got = self._sel({"v0.14.3": False, "v0.15.0-alpha.1": True, "v0.15.0-rc.1": True, "v0.15.0": False,
                         "v0.15.1-beta.1": True})
        self.assertEqual(got, ["v0.15.1-beta.1", "v0.15.0", "v0.14.3"])

    def test_prerelease_below_newer_stable_dropped(self):
        # v0.15.1 が出ている → v0.15.0-alpha.* は (同じ X.Y.Z が無くても) 古いので落とす
        got = self._sel({"v0.15.1": False, "v0.15.0-alpha.3": True})
        self.assertEqual(got, ["v0.15.1"])

    def test_malformed_skipped(self):
        got = self._sel({"v0.14.3": False, "v0.15.0-alpha1": True, "nightly": False})
        self.assertEqual(got, ["v0.14.3"])

    def test_floor_does_not_apply_to_prerelease(self):
        got = self._sel({"v0.14.3": False, "v0.15.0-alpha.1": True}, floor="v0.15.0")
        self.assertEqual(got, ["v0.15.0-alpha.1"])


class StageTest(unittest.TestCase):
    def test_outputs(self):
        gh = FakeGh({
            "v0.14.3": _rel(False, "cores3", "atoms3r", "atoms3", "stopwatch", tag="v0.14.3"),
            "v0.15.0-alpha.1": _rel(True, "cores3", "atoms3r", tag="v0.15.0-alpha.1"),
            "v0.7.0": {"prerelease": False, "assets": ["firmware-v0.7.0.zip"]},  # 旧 単一 ZIP
            "v0.6.0": _rel(False, "cores3", tag="v0.6.0"),
            "v0.14.0": {"prerelease": False, "assets": []},  # ZIP 無し
        })
        with tempfile.TemporaryDirectory() as d:
            site = Path(d)
            entries = sp.stage(gh, site)
            all_json = json.loads((site / "versions-all.json").read_text())
            stable_json = json.loads((site / "versions.json").read_text())
            self.assertEqual(entries, all_json)
            self.assertEqual([e["tag"] for e in all_json], ["v0.15.0-alpha.1", "v0.14.3", "v0.7.0"])
            self.assertEqual([e["tag"] for e in stable_json], ["v0.14.3", "v0.7.0"])
            self.assertEqual(all_json[0]["channel"], "alpha")
            self.assertTrue(all_json[0]["prerelease"])
            self.assertEqual(sorted(all_json[0]["boards"]), ["atoms3r", "cores3"])
            self.assertEqual(all_json[2]["boards"], {"cores3": "firmware-v0.7.0.zip"})
            self.assertFalse(stable_json[0]["prerelease"])
            # 旧 firmware が読む versions.json は tag / boards をそのまま持つ
            self.assertEqual(stable_json[0]["boards"]["stopwatch"], "firmware-v0.14.3-stopwatch.zip")
            # OTA 用 app イメージが取り出されている
            self.assertEqual((site / "firmware/v0.15.0-alpha.1/atoms3r/stackchan_idf.bin").read_bytes(),
                             b"v0.15.0-alpha.1:firmware-v0.15.0-alpha.1-atoms3r.zip")
            self.assertTrue((site / "firmware/v0.7.0/cores3/stackchan_idf.bin").exists())
            self.assertFalse((site / "firmware/v0.6.0").exists())
            self.assertEqual(len(gh.downloaded), 4 + 2 + 1)

    def test_board_of_asset(self):
        self.assertEqual(sp.board_of_asset("firmware-v0.14.3-stopwatch.zip"), "stopwatch")
        self.assertEqual(sp.board_of_asset("firmware-v0.15.0-alpha.1-atoms3r.zip"), "atoms3r")
        self.assertEqual(sp.board_of_asset("firmware-v0.7.0.zip"), "cores3")


if __name__ == "__main__":
    unittest.main()
