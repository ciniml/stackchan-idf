// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
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
    // リップシンク用: この区間で口が取る母音形。None = 閉口 (無音・「ん」・
    // 両唇子音の閉鎖・無声化母音)。音の合成には使わない。
    Vowel vowel = Vowel::None;
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

// 口形イベント列の組み立て (jtts.cpp)。区間を時間順に add() していくと、
// 口形が変わる所だけがイベントになる。finish() で最後が閉口でなければ
// 終端に閉口イベントを付ける。
class VisemeBuilder {
public:
    explicit VisemeBuilder(std::vector<VisemeEvent>& out) : out_(out) {}
    void add(Vowel v, float duration_ms);
    void finish();

private:
    std::vector<VisemeEvent>& out_;
    float t_ms_ = 0.0f;
    Vowel last_ = Vowel::None;
    bool have_last_ = false;
};

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

// 口形の 1 区間 (口形 + 継続時間)。HMM は音素ごとの継続長からこれを作る。
struct VisemeSpan {
    Vowel vowel = Vowel::None;
    float duration_ms = 0.0f;
};

// spans → 口形イベント列 (先頭を 0 ms とし、同じ口形は連結、最後は閉口で終わる)。
void spans_to_events(std::span<const VisemeSpan> spans, std::vector<VisemeEvent>& out);

// HMM の 1 チャンク分の受け取り側 (PCM と口形区間)。false で中断。
using ChunkFn =
    std::function<bool(std::vector<std::int16_t>&&, std::vector<VisemeSpan>&&, const std::u32string& text)>;

enum class HmmOutcome {
    Ok,        // 全チャンクを emit した
    NoOutput,  // 何も emit せずに諦めた (ボイス未ロード / メモリ不足など): 呼び出し側がフォールバック
    Aborted,   // 1 つ以上 emit した後にメモリ不足で諦めた
    Cancelled, // emit が false を返した
};

// HMM エンジン本体 (hmm_synth.cpp)。長い発話はメモリ予算に収まるチャンクに分けて
// 順に合成し、チャンクごとに emit する。
//   stream = false: 一括 (synthesize 用)。チャンクは予算いっぱいまで詰める。
//   stream = true : 低遅延 (synthesize_stream 用)。最初のチャンクを小さく、以降を
//                   徐々に大きくして、再生しながら次を合成しても途切れにくくする。
HmmOutcome render_hmm_stream(std::u32string_view text, const Options& opt, const ChunkFn& emit, bool stream);

// HMM 合成 1 回分のテキスト チャンク (hmm_chunk.cpp)。
struct HmmChunk {
    std::u32string text;
    bool pause_after = false;  // 末尾が句読点 (次のチャンクとの間に本来ポーズが入る)
    std::size_t moras = 0;
};

// text を、各チャンクが max_moras 以下になるよう句読点 / アクセント句境界
// (最後の手段でモーラ境界) で分割する。全体が収まるなら text をそのまま
// 1 チャンクにする。max_moras == 0 や発声できる内容が無いときは false。
//
// first_moras > 0 のときは低遅延モード: 最初のチャンクを first_moras 程度に抑え、
// 以降は直前のチャンクの 1.3 倍まで (max_moras を上限に) 徐々に大きくする。合成時間は
// 音声長の約 0.72 倍なので、次のチャンクの合成が前のチャンクの再生中に終わる。分割は
// 句読点 / アクセント句境界だけで行い、ここでは句の途中では切らない。
bool split_hmm_text(std::u32string_view text, std::size_t max_moras, std::vector<HmmChunk>& out,
                    std::size_t first_moras = 0);

// かな文字列 → 発話長 [ms] の粗い上限見積り (hmm_chunk.cpp)。PCM バッファの
// 確保量の事前見積りに使う。
float estimate_utterance_ms(std::u32string_view text, float mora_ms);

// samples 個の int16 PCM (+ 合成中の作業余裕) が空きメモリに収まるか。ESP では
// 空き PSRAM を見る。ホストでは常に true。収まらない発話は合成前に断り、
// std::vector の確保失敗 (例外無効なので abort) を避ける。
bool pcm_fits_in_memory(std::size_t samples);

// テスト用: PCM に使える空きメモリ [byte] を固定する (0 で実機同様に自動判定)。
void set_pcm_memory_limit_for_test(std::size_t bytes);

// テスト用: HMM 合成のメモリ予算 [byte] を固定する (0 で実機同様に自動算出 /
// ホストでは無制限)。分割合成をホストで検証するために使う。
void set_hmm_memory_budget_for_test(std::size_t bytes);

// ---- sanoTTS-jp エンジン ----

// かな文字列 (HMM と同じ記法: `'` = 直前モーラがアクセント核、`/` = アクセント
// 句境界、「、」= ポーズ、「。」= 文境界、カタカナ可) を sanoTTS-jp の
// かな中間表現 (ひらがな + `[` 上昇 `]` 下降核 `_` ポーズ) の UTF-8 に変換する
// (sano_ir.cpp)。アクセント核の無い句は平板 (2 モーラ目で上昇、下降なし)。
// 無声化 `°` は付けない (上流 M-14: 規則推定は過剰無声化する)。
// 有効なモーラが 1 つも無ければ false。
bool build_sano_ir(std::u32string_view text, std::string& ir_utf8);

// sanoTTS エンジン本体 (sano_synth.cpp)。重み未ロード・IR 変換失敗・G2P 失敗・
// arena 不足時は out を触らず false。成功時 out は 22.05 kHz mono int16 で、
// *out_rate_hz に SAAN_SR を書く。
bool render_sano(std::u32string_view text, std::vector<std::int16_t>& out, const Options& opt,
                 std::uint32_t& out_rate_hz);

}  // namespace stackchan::jtts::internal
