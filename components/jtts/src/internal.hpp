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

struct FormantFrame {
    float f1 = 500.0f, f2 = 1500.0f, f3 = 2500.0f;
    float bw1 = 70.0f, bw2 = 100.0f, bw3 = 150.0f;
    float a1 = 1.0f, a2 = 0.6f, a3 = 0.3f;
    float voicing = 1.0f;
    float frication = 0.0f;
    float f0_hz = 130.0f;
    float nasal = 0.0f;
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
