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

// チャンク単位のストリーミング合成 (HMM / sanoTTS) の結果。
enum class StreamOutcome {
    Ok,        // 全チャンクを emit した
    NoOutput,  // 何も emit せずに諦めた (ボイス未ロード / メモリ不足など): 呼び出し側がフォールバック
    Aborted,   // 1 つ以上 emit した後に失敗した (メモリ不足など)
    Cancelled, // emit が false を返した
};

// HMM エンジン本体 (hmm_synth.cpp)。長い発話はメモリ予算に収まるチャンクに分けて
// 順に合成し、チャンクごとに emit する。
//   stream = false: 一括 (synthesize 用)。チャンクは予算いっぱいまで詰める。
//   stream = true : 低遅延 (synthesize_stream 用)。最初のチャンクを小さく、以降を
//                   徐々に大きくして、再生しながら次を合成しても途切れにくくする。
StreamOutcome render_hmm_stream(std::u32string_view text, const Options& opt, const ChunkFn& emit, bool stream);

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

// ---- 句ごとの合成 (sanoTTS / フォルマント / 単位連結で共通) --------------------
//
// 発話全体を 1 回で合成すると、「、」「。」で間が入らない (フォルマント / 単位連結は
// 句読点を読み飛ばす) か、モデル任せの短いポーズになる (sanoTTS) ので、句の区切りが
// 弱い。HMM と同じく句読点 (、。，,．.) ごとに 1 句ずつ合成し、句と句の間に HMM の
// pau と同じ長さの無音を明示的に挟む。HMM はモデルが句をまとめて合成して pau を
// 生成する (その方が速く自然) ので、この経路は通さないが、区切りの定義と間の長さは
// 共有する (下の clause_pause_ms / split_clauses)。

// 句と句の間の無音 [ms]。等速 (mora_ms = 110) で 420 ms、話速に比例する (HMM の pau と
// 同じ: 両側の sil の残り 210 ms ずつ、hmm_synth.cpp)。sanoTTS の s_v / HMM の speed と
// 同じ写像で、mora_ms は [55, 220] に丸める。
inline float clause_pause_ms(float mora_ms) {
    const float scale = mora_ms / 110.0f;
    return 420.0f * (scale < 0.5f ? 0.5f : (scale > 2.0f ? 2.0f : scale));
}

// 句 (先頭から、直後の句読点までを 1 つ。続く句読点はまとめる) に分ける。区切りは
// HMM と同じ (、。，,．.)。モーラを持たない断片は前の句に含める。HmmChunk::pause_after
// は句読点で終わるか。全体にモーラが無ければ false。
bool split_clauses(std::u32string_view text, std::vector<HmmChunk>& out);

// 1 句の合成結果 (テスト用のシームでもある)。
enum class ClauseResult {
    Ok,    // pcm / rate_hz / spans を返した
    Skip,  // 読める内容が無い (この句は飛ばす)
    Fail,  // 合成失敗 (重み未ロード / G2P / arena 不足など)
};
// 1 句を合成する。spans は pcm 全体を覆う口形の区間 (母音 / 閉口 + 継続時間)。作れなければ空。
using ClauseSynth = std::function<ClauseResult(const std::u32string& clause, std::vector<std::int16_t>& pcm,
                                                   std::uint32_t& rate_hz, std::vector<VisemeSpan>& spans)>;

// 句ごとの合成の 1 チャンク (句の音声 + 句間の無音) の受け取り側。spans は pcm 全体を覆う
// 口形の区間 (句間の無音は閉口)。false で中断。
using ClauseChunkFn = std::function<bool(std::vector<std::int16_t>&& pcm, std::uint32_t rate_hz,
                                       std::vector<VisemeSpan>&& spans, const std::u32string& text)>;

// 句ごとに synth_one で合成し、句と句の間の境界を整えて emit する:
//   - 内側の境界では、句の後ろに句間の無音 (clause_pause_ms) を足す。口形の区間にも
//     閉口を足す (音と口形の時刻が揃ったまま)。
//   - trim_edges = true (sanoTTS): 内側の境界で、各句の前後の無音 (モデルが付ける) を
//     先に切り詰めて (口形の区間も同じだけ)、間の長さがモデル任せにならないようにする。
//     false (フォルマント / 単位連結): 句の音声はそのまま。
//   - 発話の先頭 / 末尾は切り詰めない。
// 最初の句が Fail なら何も出さず NoOutput (呼び出し側が他エンジンへフォールバック)、
// 途中の Fail は Aborted、Skip は飛ばす。句が 1 つだけなら加工せず、全体をそのまま渡す。
StreamOutcome stream_clauses(std::u32string_view text, const Options& opt, const ClauseSynth& synth_one,
                             const ClauseChunkFn& emit, bool trim_edges);

// trim_silence が削った量 [サンプル]。
struct SilenceTrim {
    std::size_t front = 0;
    std::size_t back = 0;
};

// pcm の前 (lead) / 後ろ (trail) の無音を切り詰める。無音 = 絶対値がピークの約 1 % (最低 48)
// 未満。音の手前 / 奥に keep_ms の余白は残す (立ち上がり / 減衰を削らないため)。
// 全体が無音なら何もしない。テスト用に公開。
SilenceTrim trim_silence(std::vector<std::int16_t>& pcm, std::uint32_t rate_hz, bool lead, bool trail,
                         std::uint32_t keep_ms = 10);

// 口形の区間列から、先頭の drop_front_ms を捨て、その後 keep_ms だけ残す (残りは捨てる)。
void crop_spans(std::vector<VisemeSpan>& spans, float drop_front_ms, float keep_ms);

// sanoTTS の音素 ID 列 (saan_g2p の出力: ^ PAD 音素 PAD 音素 ... $) と、音素ごとの継続長
// d_hat [フレーム] から、口形の区間列を作る (HMM と同じ規則):
//   母音 あ/い/う/え/お        … その母音の形
//   無声化母音・ん・っ・ポーズ   … 閉口   (ポーズ = PAD が 2 つ以上続いたときの 2 つ目以降)
//   両唇音 m b p (拗音含む)     … 閉口
//   その他の子音               … 直後の母音の形を先取り
//   PAD / 記号 [ ] # ?         … 直前の音素の形を保つ (音素の間の余白)。先頭の ^ と末尾の $ は閉口。
// ms_per_frame は 1 フレームの長さ [ms]。区間の合計 = n × ms_per_frame。
void sano_ids_to_spans(const std::int32_t* ids, const std::int32_t* d_hat, std::int32_t n, float ms_per_frame,
                       std::vector<VisemeSpan>& out);

// sanoTTS エンジン本体 (sano_synth.cpp)。stream_clauses (trim_edges) で句ごとに合成して emit する。重み未ロード
// なら NoOutput。出力は 22.05 kHz mono int16。
StreamOutcome render_sano_stream(std::u32string_view text, const Options& opt, const ClauseChunkFn& emit);

// render_sano_stream の全チャンクを 1 本に連結する (synthesize / synthesize_ex 用)。
// 重み未ロード・IR 変換失敗・G2P 失敗・arena 不足時は out を空にして false。
// 成功時 out は 22.05 kHz mono int16 で、*out_rate_hz に SAAN_SR を書く。
// visemes が非 null なら、口形イベントを追記する (out と時間軸が揃う)。
bool render_sano(std::u32string_view text, std::vector<std::int16_t>& out, const Options& opt,
                 std::uint32_t& out_rate_hz, std::vector<VisemeEvent>* visemes = nullptr);

}  // namespace stackchan::jtts::internal
