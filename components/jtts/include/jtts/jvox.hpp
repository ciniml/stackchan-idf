// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "jtts/phoneme.hpp"

// .jvox — モーラ単位 PCM + ピッチマークのパック フォーマット (単位連結 +
// TD-PSOLA エンジン用の音声 DB)。tools/jvox/pack_jvox.py が生成する。
//
//   offset  size  field
//   0       4     magic "JVOX"
//   4       1     version (= 1)
//   5       1     codec (0 = i16 LE PCM / 1 = IMA-ADPCM 4bit — 下記)
//   6       2     sample_rate (u16 LE)
//   8       2     unit_count (u16 LE)
//   10      2     reserved (0)
//   12      16×N  UnitRec 配列 (下記、key 昇順である必要はない)
//   ...     4     marks_total (u32 LE)
//   ...     2×M   marks: 全 unit のピッチマークを unit 順に連結
//                 (各値は unit 先頭からのサンプル オフセット)
//   ...     2×P   pcm: i16 サンプルを unit 順に連結
//
//   UnitRec (16 B):
//     u16 key            (unit_key() / kMoraicNKey)
//     u16 mark_count
//     u32 pcm_off        (pcm ブロック先頭からのサンプル位置)
//     u32 pcm_len        (サンプル数)
//     u16 steady_start   (unit 先頭からの母音定常部開始サンプル)
//     u16 orig_f0_dhz    (収録時平均 F0 × 10)
//
// codec = 1 (IMA-ADPCM): pcm ブロックが「u32 sample_count + 4bit ADPCM
// ニブル列 (下位ニブル→上位ニブル、predictor=0/index=0 開始の単一ストリーム)」
// に置き換わる。フラッシュ (NVS blob 上限 508 KB) に収めるための転送/保存
// 形式で、実行前に decode_adpcm() で codec=0 の生形式へ展開してから
// Db::parse() する。
//
// バージョン/コーデックの追加は append-only (既存フィールドの意味を変えない)。

namespace stackchan::jtts::jvox {

// CV モーラの unit キー。Consonant/Vowel の enum 値に依存するが、この enum は
// append-only (kana_table が参照) なので既存キーは安定。
constexpr std::uint16_t unit_key(Consonant c, Vowel v, bool palatalized) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(c) << 8) |
                                      (static_cast<unsigned>(v) << 1) |
                                      (palatalized ? 1u : 0u));
}
constexpr std::uint16_t unit_key(const Mora& m) {
    return unit_key(m.c, m.v, m.palatalized);
}
// 「ん」(MoraicN) 用の特殊キー。
inline constexpr std::uint16_t kMoraicNKey = 0xFFFE;

struct UnitView {
    std::span<const std::int16_t> pcm;      // i16 LE (アライン済み前提: 下記 Db 参照)
    std::span<const std::uint16_t> marks;   // unit 先頭からのサンプル オフセット
    std::uint16_t steady_start = 0;
    float orig_f0_hz = 0.0f;
};

// 読み取り専用ビュー。blob (flash mmap / PSRAM) の寿命は呼び出し側が保証する。
// blob 先頭は 2 バイト アラインであること (pcm/marks を reinterpret するため)。
class Db {
public:
    // パース + 全レコードの範囲検証。失敗時は valid() == false。
    static std::optional<Db> parse(std::span<const std::uint8_t> blob);

    std::uint32_t sample_rate() const { return sample_rate_; }
    std::uint16_t unit_count() const { return unit_count_; }

    // key の unit を返す。無ければ nullopt。
    std::optional<UnitView> find(std::uint16_t key) const;

private:
    std::span<const std::uint8_t> blob_;
    std::uint32_t sample_rate_ = 0;
    std::uint16_t unit_count_ = 0;
    const std::uint8_t* units_ = nullptr;      // UnitRec 配列先頭
    const std::uint16_t* marks_ = nullptr;     // marks ブロック先頭
    std::uint32_t marks_total_ = 0;
    const std::int16_t* pcm_ = nullptr;        // pcm ブロック先頭
    std::uint32_t pcm_total_ = 0;
};

// --- ADPCM (codec=1) の展開 -------------------------------------------------

// codec=1 の blob か (magic/version も検査)。
bool is_adpcm(std::span<const std::uint8_t> blob);

// 展開後 (codec=0) のバイト数。blob が不正なら 0。
std::size_t decoded_size(std::span<const std::uint8_t> blob);

// codec=1 blob を codec=0 blob へ展開する。out.size() は decoded_size() 丁度
// であること (呼び出し側が PSRAM 等に確保する)。成功で true。
bool decode_adpcm(std::span<const std::uint8_t> blob, std::span<std::uint8_t> out);

}  // namespace stackchan::jtts::jvox
