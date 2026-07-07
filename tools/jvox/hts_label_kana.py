# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""かな入力のみから HTS full-context ラベルを生成する (辞書・アクセント推定なし)。

実機 (ESP32-S3) に hts_engine を載せる場合に到達できる品質上限を
ホスト上で検証するためのリファレンス実装。前提:
  - 形態素解析なし → 品詞系フィールド (B/C/D/E の大半) は xx
  - アクセント推定なし → 全アクセント句を平板型 (type 0) とする
  - アクセント句境界は読点 (、。) のみ = 呼気段落 1 つ = アクセント句 1 つ
    (オプションで「'」によるアクセント核指定と「/」による句切り指定に対応)

音素列は pyopenjtalk.g2p(かな) で得る (ひらがな入力なら辞書に依存しない
決定的変換で、実機の parse_kana + apply_devoicing と同等)。
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

VOWELS = {"a", "i", "u", "e", "o", "A", "I", "U", "E", "O"}
MORA_SOLO = {"N", "cl"}  # 撥音・促音は単独で 1 モーラ


@dataclass
class Mora:
    phonemes: list[str]  # [consonant?, vowel] or ["N"] / ["cl"]


@dataclass
class AccentPhrase:
    morae: list[Mora] = field(default_factory=list)
    accent: int = 0  # 0 = 平板
    interrogative: bool = False


@dataclass
class BreathGroup:
    phrases: list[AccentPhrase] = field(default_factory=list)

    def mora_count(self) -> int:
        return sum(len(p.morae) for p in self.phrases)


def phonemes_to_morae(phonemes: list[str]) -> list[Mora]:
    morae: list[Mora] = []
    pending: list[str] = []
    for ph in phonemes:
        if ph in MORA_SOLO:
            assert not pending, f"consonant before {ph}: {pending}"
            morae.append(Mora([ph]))
        elif ph in VOWELS:
            morae.append(Mora(pending + [ph]))
            pending = []
        else:
            assert not pending, f"consonant cluster: {pending} + {ph}"
            pending = [ph]
    assert not pending, f"trailing consonant: {pending}"
    return morae


def parse_kana(text: str, g2p) -> tuple[list[BreathGroup], bool]:
    """かな文字列 → 呼気段落/アクセント句構造。

    「、」「。」で呼気段落を切る。呼気段落内は「/」でアクセント句を切れる
    (無ければ 1 句)。「'」は直前モーラをアクセント核にする (無ければ平板)。
    """
    interrogative = text.rstrip().endswith(("？", "?"))
    text = re.sub(r"[？?！!。]+$", "", text.strip())
    groups: list[BreathGroup] = []
    for chunk in re.split(r"[、。，,]+", text):
        chunk = chunk.strip()
        if not chunk:
            continue
        bg = BreathGroup()
        for ap_text in chunk.split("/"):
            ap_text = ap_text.strip()
            if not ap_text:
                continue
            # 「'」の位置 = アクセント核のモーラ番号 (1-based)
            accent = 0
            plain = ""
            for ch in ap_text:
                if ch == "'":
                    # ここまでのモーラ数が核位置
                    ph = g2p(plain).split()
                    accent = len(phonemes_to_morae([p for p in ph if p != "pau"]))
                else:
                    plain += ch
            phonemes = [p for p in g2p(plain).split() if p != "pau"]
            ap = AccentPhrase(morae=phonemes_to_morae(phonemes), accent=accent)
            bg.phrases.append(ap)
        if bg.phrases:
            groups.append(bg)
    if groups and interrogative:
        groups[-1].phrases[-1].interrogative = True
    return groups, interrogative


def build_labels(groups: list[BreathGroup]) -> list[str]:
    """HTS 日本語標準 full-context ラベルを生成する。

    品詞 (B/C/D)・語境界系は xx。位置系フィールドは正確に埋める。
    """
    # 発話全体の集計
    total_bg = len(groups)
    total_ap = sum(len(bg.phrases) for bg in groups)
    total_mora = sum(bg.mora_count() for bg in groups)
    k = f"/K:{total_bg}+{total_ap}-{total_mora}"

    # 音素の平坦列 (sil / pau 込み) を作りつつ、各音素のコンテキストを記録
    entries: list[dict] = []  # {ph, ctx...}

    def ap_ctx(ap: AccentPhrase | None) -> tuple[str, str, str]:
        """(#morae, accent, interrogative) を文字列で"""
        if ap is None:
            return "xx", "xx", "xx"
        return str(len(ap.morae)), str(ap.accent), "1" if ap.interrogative else "0"

    # AP の発話内通し番号
    ap_flat: list[tuple[int, int]] = []  # (bg_idx, ap_idx)
    for gi, bg in enumerate(groups):
        for pi in range(len(bg.phrases)):
            ap_flat.append((gi, pi))

    mora_global = 0  # 発話内モーラ通し番号 (0-based)
    entries.append({"ph": "sil", "sil": True})
    for gi, bg in enumerate(groups):
        bg_mora = bg.mora_count()
        # 呼気段落内の AP 開始モーラ位置
        mora_in_bg = 0
        for pi, ap in enumerate(bg.phrases):
            n_mora = len(ap.morae)
            acc = ap.accent
            ap_global = ap_flat.index((gi, pi))
            for mi, mora in enumerate(ap.morae):
                pos = mi + 1  # 1-based mora position in AP
                a1 = pos - acc if acc > 0 else pos - n_mora - 1  # 平板は核なし → 全て負側
                # OpenJTalk の平板 (type0) の a1 は pos - 0 ではなく…実測に合わせ pos-acc、
                # type0 のとき慣例的に a1 = pos - (n_mora+1) とする実装が多いが、
                # ここでは HTS 質問が主に「核より前/後」を見るため type0 は全モーラ核前扱い。
                for ph in mora.phonemes:
                    entries.append(
                        {
                            "ph": ph,
                            "sil": False,
                            "a1": a1,
                            "a2": pos,
                            "a3": n_mora - pos + 1,
                            "f": (n_mora, acc, ap.interrogative),
                            "f5": pi + 1,
                            "f6": len(bg.phrases) - pi,
                            "f7": mora_in_bg + mi + 1,
                            "f8": bg_mora - (mora_in_bg + mi),
                            "gi": gi,
                            "pi": pi,
                            "ap_global": ap_global,
                            "mora_global": mora_global + mi,
                        }
                    )
            mora_in_bg += n_mora
        mora_global += bg_mora
        if gi != len(groups) - 1:
            entries.append({"ph": "pau", "sil": True, "gi": gi})
    entries.append({"ph": "sil", "sil": True})

    # 前後 AP コンテキスト (E/G) と呼気段落 (H/I/J) を引くヘルパ
    def get_ap(idx: int) -> AccentPhrase | None:
        if 0 <= idx < len(ap_flat):
            gi, pi = ap_flat[idx]
            return groups[gi].phrases[pi]
        return None

    labels: list[str] = []
    phs = [e["ph"] for e in entries]
    for i, e in enumerate(entries):
        p1 = phs[i - 2] if i >= 2 else "xx"
        p2 = phs[i - 1] if i >= 1 else "xx"
        p3 = phs[i]
        p4 = phs[i + 1] if i + 1 < len(phs) else "xx"
        p5 = phs[i + 2] if i + 2 < len(phs) else "xx"
        quin = f"{p1}^{p2}-{p3}+{p4}={p5}"

        if e["sil"]:
            # sil/pau: 前後の AP を E/G に、A/F は xx
            if e["ph"] == "sil" and i == 0:
                nxt = get_ap(0)
                g1, g2, g3 = ap_ctx(nxt)
                lab = (
                    f"{quin}/A:xx+xx+xx/B:xx-xx_xx/C:xx_xx+xx/D:xx+xx_xx"
                    f"/E:xx_xx!xx_xx-xx/F:xx_xx#xx_xx@xx_xx|xx_xx"
                    f"/G:{g1}_{g2}%{g3}_xx_xx/H:xx_xx"
                    f"/I:xx-xx@xx+xx&xx-xx|xx+xx/J:{len(groups[0].phrases)}_{groups[0].mora_count()}{k}"
                )
            elif e["ph"] == "sil":
                prev = get_ap(total_ap - 1)
                e1, e2, e3 = ap_ctx(prev)
                lab = (
                    f"{quin}/A:xx+xx+xx/B:xx-xx_xx/C:xx_xx+xx/D:xx+xx_xx"
                    f"/E:{e1}_{e2}!{e3}_xx-xx/F:xx_xx#xx_xx@xx_xx|xx_xx"
                    f"/G:xx_xx%xx_xx_xx/H:{len(groups[-1].phrases)}_{groups[-1].mora_count()}"
                    f"/I:xx-xx@xx+xx&xx-xx|xx+xx/J:xx_xx{k}"
                )
            else:  # pau
                gi = e["gi"]
                prev_ap = groups[gi].phrases[-1]
                next_ap = groups[gi + 1].phrases[0]
                e1, e2, e3 = ap_ctx(prev_ap)
                g1, g2, g3 = ap_ctx(next_ap)
                lab = (
                    f"{quin}/A:xx+xx+xx/B:xx-xx_xx/C:xx_xx+xx/D:xx+xx_xx"
                    f"/E:{e1}_{e2}!{e3}_xx-xx/F:xx_xx#xx_xx@xx_xx|xx_xx"
                    f"/G:{g1}_{g2}%{g3}_xx_xx/H:{len(groups[gi].phrases)}_{groups[gi].mora_count()}"
                    f"/I:xx-xx@xx+xx&xx-xx|xx+xx/J:{len(groups[gi+1].phrases)}_{groups[gi+1].mora_count()}{k}"
                )
            labels.append(lab)
            continue

        gi, pi = e["gi"], e["pi"]
        bg = groups[gi]
        n_mora, acc, inter = e["f"]
        f3 = "1" if inter else "0"
        prev_ap = get_ap(e["ap_global"] - 1)
        next_ap = get_ap(e["ap_global"] + 1)
        e1, e2, e3 = ap_ctx(prev_ap)
        g1, g2, g3 = ap_ctx(next_ap)
        # E5: 直前 AP との間にポーズがあるか (別 BG なら 1)
        e5 = "xx"
        if prev_ap is not None:
            e5 = "1" if ap_flat[e["ap_global"] - 1][0] != gi else "0"
        g5 = "xx"
        if next_ap is not None:
            g5 = "1" if ap_flat[e["ap_global"] + 1][0] != gi else "0"

        h = f"{len(groups[gi-1].phrases)}_{groups[gi-1].mora_count()}" if gi > 0 else "xx_xx"
        j = (
            f"{len(groups[gi+1].phrases)}_{groups[gi+1].mora_count()}"
            if gi + 1 < len(groups)
            else "xx_xx"
        )
        # I: 現呼気段落
        aps_before = sum(len(g.phrases) for g in groups[:gi])
        morae_before = sum(g.mora_count() for g in groups[:gi])
        i_field = (
            f"{len(bg.phrases)}-{bg.mora_count()}"
            f"@{gi+1}+{len(groups)-gi}"
            f"&{aps_before+pi+1}-{total_ap-(aps_before+pi)}"
            f"|{morae_before+ (e['f7'])}+{total_mora-(morae_before+e['f7']-1)}"
        )
        lab = (
            f"{quin}/A:{e['a1']}+{e['a2']}+{e['a3']}"
            f"/B:xx-xx_xx/C:xx_xx+xx/D:xx+xx_xx"
            f"/E:{e1}_{e2}!{e3}_xx-{e5}"
            f"/F:{n_mora}_{acc}#{f3}_xx@{e['f5']}_{e['f6']}|{e['f7']}_{e['f8']}"
            f"/G:{g1}_{g2}%{g3}_xx_{g5}"
            f"/H:{h}"
            f"/I:{i_field}"
            f"/J:{j}{k}"
        )
        labels.append(lab)
    return labels


def kana_to_labels(text: str, g2p) -> list[str]:
    groups, _ = parse_kana(text, g2p)
    return build_labels(groups)


if __name__ == "__main__":
    import sys

    import pyopenjtalk

    text = sys.argv[1] if len(sys.argv) > 1 else "こんにちは、ぼくはすたっくちゃんです"
    for lab in kana_to_labels(text, pyopenjtalk.g2p):
        print(lab)
