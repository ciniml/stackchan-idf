// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "jtts/jtts.hpp"
#include "jtts/phoneme.hpp"

namespace stackchan::jtts::internal {

// V2 レンダラの鼻音極 (固定)。鼻音ゼロは nasal=0 のときこの周波数に置かれて
// 極と厳密に打ち消し合う (Klatt 流)。FormantFrame::nasal_zero_hz の既定値も
// これに合わせておくと、非鼻音フレームとの補間中もゼロが極位置から動かない。
constexpr float kNasalPoleHz = 280.0f;
constexpr float kNasalBwHz = 100.0f;

// 摩擦ノイズのスペクトル整形クラス (V2 のみ)。調音位置ごとにノイズの
// 帯域が違う: 歯茎の /s z ts/ は 4 kHz 以上、後部歯茎の /sh ch j/ は
// 2.5–3.5 kHz。Formant は従来通り母音フォルマント BPF で整形 (/h/ 系)。
enum class FricationShape : std::uint8_t {
    Formant = 0,   // 母音フォルマント BPF ×3 (従来動作)
    Sibilant = 1,  // /s z ts/: HPF 4 kHz + 6.5 kHz 広帯域ピーク
    Palatal = 2,   // /sh ch j/: HPF 2 kHz + 3 kHz 帯域 (bw 1500)
};

struct FormantFrame {
    float f1 = 500.0f, f2 = 1500.0f, f3 = 2500.0f;
    float bw1 = 70.0f, bw2 = 100.0f, bw3 = 150.0f;
    float a1 = 1.0f, a2 = 0.6f, a3 = 0.3f;
    float voicing = 1.0f;
    float frication = 0.0f;
    float f0_hz = 130.0f;
    // 鼻音化度 0..1。0 = 鼻音ゼロが極を打ち消して透過、1 = ゼロが
    // nasal_zero_hz まで移動して口腔外の反共振が現れる。
    float nasal = 0.0f;
    // nasal > 0 のときの鼻音ゼロの目標周波数 [Hz] (/m/=1000, /n/=1500,
    // /N/(ん)=1800)。既定は極位置 (= 打ち消し) にしておき、補間で乱れない
    // ようにする。
    float nasal_zero_hz = kNasalPoleHz;
    // 帯気 0..1。声帯振動なしでカスケード (後続母音のフォルマント) を
    // ノイズ駆動する。無声破裂音の VOT 区間で使う。
    float aspiration = 0.0f;
    // 摩擦ノイズの整形クラス (セグメント単位、start 側の値を使う)。
    FricationShape fric_shape = FricationShape::Formant;
};

struct Segment {
    FormantFrame start;
    FormantFrame end;
    float duration_ms = 0.0f;
};

bool parse_kana(std::u32string_view kana, std::vector<Mora>& out);

// 東京式無声化: /i/ /u/ が無声子音の間または無声子音+文末で囁かれる。
// 該当する Mora の devoiced フラグを立てる。
void apply_devoicing(std::vector<Mora>& moras);

// 句レベルの F0 輪郭 (句頭上昇 → 漸降 → 文末降下)。base F0 に掛ける倍率を
// 発話内時刻から返す。フォルマント/単位連結の両エンジンで共有する。
class ProsodyCurve {
public:
    explicit ProsodyCurve(float total_ms);
    float at(float t_ms) const;

private:
    float total_ms_;
    float rise_ms_;
    float fall_start_ms_;
    float fall_ms_;
};

// ProsodyCurve をセグメント列の F0 に適用する (フォルマント エンジン用)。
void apply_prosody(std::vector<Segment>& segs, const Options& opt);

void build_segments(std::span<const Mora> moras, std::span<Segment> /*unused-placeholder*/);
void build_segments(std::span<const Mora> moras, std::vector<Segment>& out, const Options& opt);

FormantFrame vowel_frame(Vowel v, bool palatalized);
FormantFrame nasal_frame(Consonant c);
FormantFrame consonant_burst(Consonant c, Vowel next_v);

// Options::synth で V2 / Classic をディスパッチする。
void render_segments(std::span<const Segment> segs, std::vector<std::int16_t>& out, const Options& opt);
// Classic バリアント本体 (formant_synth_classic.cpp)。
void render_segments_classic(std::span<const Segment> segs, std::vector<std::int16_t>& out,
                             const Options& opt);

}  // namespace stackchan::jtts::internal

namespace stackchan::jtts::jvox {
class Db;
}

namespace stackchan::jtts::internal {

// 単位連結 + TD-PSOLA エンジン (unit_synth.cpp)。必要な単位が DB に揃って
// いれば out に追記して true。欠けがあれば out を触らず false (呼び出し側が
// フォルマント エンジンへフォールバックする)。
bool render_units(std::span<const Mora> moras, const jvox::Db& db,
                  std::vector<std::int16_t>& out, const Options& opt);

// ---- HMM (hts_engine) エンジン ----

// かな文字列 (アクセント記号 `'` = 直前モーラが核、`/` = アクセント句境界、
// 「、」「。」= 呼気段落境界) から HTS full-context ラベルを生成する
// (hts_label.cpp)。品詞情報なし・無指定アクセントは平板型。
// 検証リファレンス: tools/jvox/hts_label_kana.py
bool build_hts_labels(std::u32string_view text, std::vector<std::string>& labels);

// HMM エンジン本体 (hmm_synth.cpp)。ボイス未ロード・ラベル生成失敗・
// レート非対応時は out を触らず false (呼び出し側がフォールバック)。
bool render_hmm(std::u32string_view text, std::vector<std::int16_t>& out, const Options& opt);

}  // namespace stackchan::jtts::internal
