# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""pack_jvox: モーラ単位 WAV 群から .jvox (単位連結 TTS 用音声 DB) を作る。

Usage:
    python3 pack_jvox.py [--adpcm] <unit_dir> <out.jvox>

<unit_dir> には index.tsv (key_hex \t kana \t filename [\t f0_hint]) と
16 kHz mono i16 の WAV 群を置く。各単位についてピッチマーク (声門閉鎖近傍の
負ピーク列) を抽出してパックする。フォーマットは
components/jtts/include/jtts/jvox.hpp のコメントが正。

--adpcm: pcm ブロックを IMA-ADPCM 4bit (codec=1) で格納する。実機への転送/
NVS 保存用 (~1/4 サイズ、NVS blob 上限 508 KB に収まる)。デバイスはロード時
に codec=0 へ展開する。
"""

import struct
import sys
import wave
from pathlib import Path

import numpy as np


def load_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path)) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2, path
        fs = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return x.astype(np.float64), fs


def norm_autocorr(seg: np.ndarray) -> np.ndarray:
    """ラグ長で正規化した自己相関 (score[lag] ≈ 相関係数)。

    無正規化の np.correlate は長いラグほど重なりサンプル数が減って値が
    系統的に小さくなり、低い男声 (周期 ~140 サンプル @16k) の真の周期より
    倍音側のラグを拾ってしまう (nitech m001 で被覆率崩壊を実測)。
    """
    n = len(seg)
    ac = np.correlate(seg, seg, "full")[n - 1 :]
    denom = ac[0] * (np.arange(n, 0, -1) / n)
    return ac / (denom + 1e-9)


def estimate_f0(x: np.ndarray, fs: int, lo=70.0, hi=450.0) -> float:
    """フレームごとの自己相関 F0 の中央値。無声なら 0。

    全体一括の自己相関だと、無声子音が長い単位 (「しー」等) で周期性が
    薄まって推定に失敗する。30 ms フレームで周期性の高い (正規化相関 >0.5)
    フレームだけ集めて中央値を取る。
    """
    fl = int(fs * 0.040)
    lag_lo, lag_hi = int(fs / hi), int(fs / lo)
    f0s = []
    for i in range(0, len(x) - fl, fl // 2):
        seg = x[i : i + fl] - x[i : i + fl].mean()
        e = float((seg * seg).sum())
        if e < 1.0:
            continue
        sc = norm_autocorr(seg)
        hi_l = min(lag_hi, len(sc) - 1)
        if hi_l <= lag_lo:
            continue
        pk = lag_lo + int(np.argmax(sc[lag_lo:hi_l]))
        if sc[pk] > 0.5:
            f0s.append(fs / pk)
    if not f0s:
        return 0.0
    return float(np.median(f0s))


def pitch_marks(x: np.ndarray, fs: int, f0: float) -> list[int]:
    """周期ごとの負ピーク (声門閉鎖近傍) を前方走査でスナップして返す。

    有声区間の判定は「周期窓内のエネルギーがピーク値の 5% 以上」で緩く取り、
    先頭の子音部 (無声) はマークなしのまま残す。
    """
    if f0 <= 0:
        return []
    period = int(round(fs / f0))
    if period < 8:
        return []
    env = np.abs(x)
    # 移動 RMS でエネルギー包絡を出し、有声しきい値を決める。
    k = period
    kernel = np.ones(k) / k
    rms = np.sqrt(np.convolve(x * x, kernel, "same") + 1e-9)
    thresh = rms.max() * 0.10

    marks: list[int] = []
    # 開始点は「音量がある」ではなく「周期性がある」最初のフレーム。
    # 音量基準だと無声破裂・摩擦の騒音部から歩き始めてしまい、子音と母音の
    # 間の谷で打ち切られる (き・し 等で実測)。子音部はマークなし = verbatim
    # ヘッドとして残るのが正しい。歩進周期はそのフレームの局所周期から
    # 始める (単発モーラは単位内で F0 が滑るので、単位中央値との一致は
    # 要求しない — 適応歩進が追従する)。
    fl = int(fs * 0.040)
    p_min, p_max = int(fs / 450), int(fs / 70)
    # 開始スキャンの探索範囲は単位中央値 F0 の ±1 オクターブに絞る。
    # 全域だと無声破裂の気息音が探索下限の縁 (高い擬似周期) にマッチして
    # しまい、誤った周期でアンカーして歩進が即死する (nitech の け/きょ 等で
    # 実測)。
    s_min = max(p_min, int(fs / (f0 * 2.0)))
    s_max = min(p_max, int(fs / (f0 * 0.5)))
    pos = -1
    for i in range(0, len(x) - fl, fl // 2):
        seg = x[i : i + fl] - x[i : i + fl].mean()
        e = float((seg * seg).sum())
        if e < 1.0 or rms[min(i + fl // 2, len(rms) - 1)] <= thresh:
            continue
        sc = norm_autocorr(seg)
        hi_l = min(s_max, len(sc) - 1)
        if hi_l <= s_min:
            continue
        pk = s_min + int(np.argmax(sc[s_min:hi_l]))
        if sc[pk] > 0.5:
            pos = i + fl // 2  # rms 検査を通ったフレーム中点をアンカーにする
            period = pk  # 局所周期で歩き始める
            break
    if pos < 0:
        return []
    # 最初のマーク: pos から 1.5 周期以内の最小値。
    w_end = min(len(x), pos + period + period // 2)
    if w_end - pos < period // 2:
        return []
    pos = pos + int(np.argmin(x[pos:w_end]))
    marks.append(pos)
    # 歩進周期は直近のマーク間隔で適応させる (単発モーラ発話は単位内で
    # F0 が 25% 以上滑ることがあり、固定周期だと途中で外れる)。
    # 有声区間内の短い谷 (有声子音のわたり: ら行のはじき、ば行の閉鎖など)
    # は最大 50 ms までマークなしで前進し、回復したら再アンカーする —
    # 谷で打ち切ると母音側が丸ごとマークなしになる (べ・ら・る で実測)。
    cur = float(period)
    max_gap = int(fs * 0.050)
    anchor = marks[-1]  # 次のマーク探索の基準 (谷スキップ中も前進する)
    gap = 0
    while True:
        p = int(round(cur))
        center = anchor + p
        lo = center - p // 3
        hi = center + p // 3
        if hi >= len(x):
            break
        if rms[min(center, len(rms) - 1)] <= thresh:
            gap += p
            if gap > max_gap:
                break  # 有声区間の終わり
            anchor = center  # 谷を素通り (マークは打たない)
            continue
        m = lo + int(np.argmin(x[lo:hi]))
        if m <= anchor:
            break
        if gap == 0 and marks:
            cur = min(max(0.7 * cur + 0.3 * (m - marks[-1]), p_min), p_max)
        gap = 0
        anchor = m
        marks.append(m)
    # 語尾のきしみ声 (creak) 刈り: 男声は単発発話の末尾で F0 が 70 Hz 台まで
    # 落ち、その区間のグレインを合成に使うとモーラ末が濁って途切れ感になる
    # (nitech m001 で実測)。マーク間隔が中央値の 1.6 倍を超える末尾を捨てる。
    if len(marks) >= 5:
        iv = np.diff(marks)
        med = float(np.median(iv))
        while len(marks) >= 5 and marks[-1] - marks[-2] > 1.6 * med:
            marks.pop()
    # u16 に収まることを検査 (unit ≤ ~65k サンプル)。
    if marks and marks[-1] > 0xFFFF:
        raise ValueError("unit too long for u16 pitch marks")
    return marks


def steady_start(marks: list[int]) -> int:
    """母音定常部の開始 (将来のクロスフェード用)。マーク列の 40% 地点。"""
    if not marks:
        return 0
    return marks[max(0, int(len(marks) * 0.4))]


def ref_rms(x: np.ndarray, marks: list[int]) -> float:
    """有声部の代表 RMS (エンジンの unit_ref_rms と同じ定義: 前半 70% の中央値)。"""
    if len(marks) < 3:
        return 0.0
    n = max(1, len(marks) * 7 // 10)
    vals = []
    for j in range(1, min(len(marks) - 1, n + 1)):
        p = marks[j] - marks[j - 1]
        lo, hi = max(0, marks[j] - p), min(len(x), marks[j] + p + 1)
        vals.append(float(np.sqrt((x[lo:hi] ** 2).mean())))
    return float(np.median(vals)) if vals else 0.0


def normalize_unit(x: np.ndarray, marks: list[int]) -> np.ndarray:
    """単位間のレベルを揃える。

    生成側 (gen_units_openjtalk) はピーク正規化なので、摩擦音がピークの単位
    (「し」等) は子音が突出し、母音の基準レベルは単位ごとにバラバラになる —
    連結すると「境界の段差」と「ノイズっぽい子音」になる (実聴で確認)。
    有声部の代表 RMS を全単位で共通の目標値に揃える (単位内の子音/母音
    バランスは元発話のまま保たれる)。無声単位はピーク基準。
    """
    target = 3000.0  # 共通の母音基準 RMS (i16)
    r = ref_rms(x, marks)
    if r > 1.0:
        scale = target / r
    else:
        peak = float(np.abs(x).max())
        scale = (0.35 * 32767.0 / peak) if peak > 0 else 1.0
    peak = float(np.abs(x).max())
    if peak * scale > 0.95 * 32767.0:
        scale = 0.95 * 32767.0 / peak
    return x * scale


IMA_INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
IMA_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
]


def adpcm_encode(pcm: np.ndarray) -> bytes:
    """i16 列を IMA-ADPCM 4bit (単一ストリーム、predictor=0/index=0 開始) に。

    デコーダは components/jtts/src/jvox_adpcm.cpp — 両者は同じ量子化規則
    (diff 再構成方式) を使うので往復のドリフトはステップ量子化誤差のみ。
    """
    pred, index = 0, 0
    nibbles = bytearray()
    cur = 0
    for i, s in enumerate(np.asarray(pcm, dtype=np.int64)):
        step = IMA_STEP_TABLE[index]
        diff = int(s) - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
        if diff >= step >> 2:
            code |= 1
        # デコーダと同じ再構成
        rec = step >> 3
        if code & 1:
            rec += step >> 2
        if code & 2:
            rec += step >> 1
        if code & 4:
            rec += step
        if code & 8:
            rec = -rec
        pred = max(-32768, min(32767, pred + rec))
        index = max(0, min(88, index + IMA_INDEX_TABLE[code]))
        if i % 2 == 0:
            cur = code
        else:
            nibbles.append(cur | (code << 4))
    if len(pcm) % 2 == 1:
        nibbles.append(cur)
    return bytes(nibbles)


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--adpcm"]
    use_adpcm = "--adpcm" in sys.argv[1:]
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 1
    unit_dir = Path(args[0])
    out_path = Path(args[1])

    entries = []
    for line in (unit_dir / "index.tsv").read_text().splitlines():
        parts = line.strip().split("\t")
        if len(parts) < 3:
            continue
        entries.append((int(parts[0], 16), parts[1], parts[2]))

    sample_rate = None
    units = []  # (key, pcm int16 array, marks, steady, f0)
    for key, kana, fname in entries:
        x, fs = load_wav(unit_dir / fname)
        if sample_rate is None:
            sample_rate = fs
        assert fs == sample_rate, f"sample-rate mismatch: {fname}"
        f0 = estimate_f0(x, fs)
        marks = pitch_marks(x, fs, f0)
        if len(marks) < 3:
            print(f"  [warn] {kana} ({fname}): voiced marks={len(marks)} f0={f0:.0f} — "
                  "無声単位として格納 (verbatim 再生)")
            marks = []
        x = normalize_unit(x, marks)
        units.append((key, x.astype(np.int16), marks, steady_start(marks), f0))
        print(f"  {kana:4s} key={key:04x} len={len(x):6d} f0={f0:6.1f} marks={len(marks)}")

    # パック
    marks_all: list[int] = []
    pcm_all: list[np.ndarray] = []
    recs = []
    pcm_off = 0
    for key, pcm, marks, steady, f0 in units:
        recs.append(struct.pack("<HHIIHH", key, len(marks), pcm_off, len(pcm),
                                steady, int(round(f0 * 10))))
        marks_all.extend(marks)
        pcm_all.append(pcm)
        pcm_off += len(pcm)

    codec = 1 if use_adpcm else 0
    blob = b"JVOX" + struct.pack("<BBHHH", 1, codec, sample_rate, len(units), 0)
    blob += b"".join(recs)
    blob += struct.pack("<I", len(marks_all))
    blob += np.asarray(marks_all, dtype="<u2").tobytes()
    pcm_cat = np.concatenate(pcm_all).astype("<i2")
    if use_adpcm:
        blob += struct.pack("<I", len(pcm_cat))
        blob += adpcm_encode(pcm_cat)
    else:
        blob += pcm_cat.tobytes()

    out_path.write_bytes(blob)
    print(f"wrote {out_path} ({len(blob)} bytes, {len(units)} units, "
          f"{sum(len(p) for p in pcm_all)} samples)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
