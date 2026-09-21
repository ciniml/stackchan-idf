// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "jtts/phoneme.hpp"

namespace stackchan::jtts {

enum class Voice : std::uint8_t {
    Male,    // 大人男性 (F0 ≈ 130 Hz、フォルマント等倍)
    Female,  // 大人女性 (F0 ≈ 210 Hz、フォルマントを ~17% 持ち上げ)
};

// フォルマント エンジンの合成方式バリアント。
//   V2      — 声門波励起 + カスケード声道 (a14bd75 以降の既定)
//   Classic — インパルス列 + 並列 BPF×3 (それ以前の実装。ロボットらしい
//             ブザー声が好みの場合に選ぶ)
// 将来の単位連結エンジン (Engine 軸、docs/jtts-unit-tts-research.md) とは
// 直交する軸。
enum class SynthVariant : std::uint8_t {
    V2 = 0,
    Classic = 1,
};

// 合成エンジンの選択 (SynthVariant はフォルマント エンジン内の音色バリアント、
// こちらはエンジンそのものの軸)。
//   Auto    — HMM ボイス (set_hmm_voice) があれば Hmm、次に音声 DB
//             (set_voice_db) があれば Unit、どちらも無ければ Formant
//   Formant — 常にフォルマント合成
//   Unit    — 単位連結 (TD-PSOLA)。DB 未ロード / 必要単位の欠けは
//             Formant へ自動フォールバック
//   Hmm     — HMM 合成 (hts_engine)。ボイス未ロード時は Unit → Formant へ
//             フォールバック
//   Sano    — sanoTTS-jp (ニューラル、22.05 kHz)。重み未ロード時は Hmm →
//             Unit → Formant へフォールバック。Auto では Hmm より優先。
enum class Engine : std::uint8_t {
    Auto = 0,
    Formant = 1,
    Unit = 2,
    Hmm = 3,
    Sano = 4,
};

struct Options {
    std::uint32_t sample_rate_hz = 16000;
    Voice voice = Voice::Male;
    // 0 を指定すると voice のデフォルトを使う。明示すると上書き。
    float f0_hz = 0.0f;
    float formant_scale = 0.0f;
    float mora_ms = 110.0f;
    float gain = 0.6f;

    // ----- 声色 (timbre) -----
    // 声帯駆動成分にノイズをどれだけ混ぜるか。0=純声、1=完全に息のみ。
    float breathiness = 0.0f;
    // 有声成分の全体スケール。0 にすると囁き声 (breathiness=1 と併用)。
    float voicing_mul = 1.0f;
    // 無声 (摩擦) 成分の全体スケール。
    float frication_mul = 1.0f;
    // F0 ビブラート (0 = OFF)。rate は LFO 周波数 Hz、depth はセント単位
    // (100 セント = 1 半音)。老人の震え声には rate≈4-5, depth≈30-50 が良い。
    float vibrato_rate_hz = 0.0f;
    float vibrato_cents = 0.0f;

    // ----- やわらかさ (V2 のみ、Classic では無視) -----
    // 声門開大比 (open quotient)。上げると閉鎖が弱くなり高域が減って
    // やわらかい発声になる。0.35–0.85、既定 0.56 (従来と同じ音)。
    float glottal_oq = 0.56f;
    // スペクトル傾斜: 3 kHz での追加減衰量 [dB] (1-pole LPF)。0 = OFF。
    // やわらかめは 6–12。Klatt 合成器の TL パラメータ相当。
    float tilt_db = 0.0f;
    // フォルマント帯域幅の倍率。上げると共鳴ピークが鈍り金属的な鳴きが
    // 減る。0.7–2.0、既定 1.0。(Classic でも有効)
    float bw_scale = 1.0f;
    // 合成方式。既定 V2。
    SynthVariant synth = SynthVariant::V2;
    // エンジン選択。既定 Auto (HMM ボイス > 音声 DB > フォルマントの順)。
    Engine engine = Engine::Auto;

    // ----- HMM エンジンのみ -----
    // ピッチシフト [半音]。ボイス既定ピッチからの相対 (+ で高く)。
    float hmm_half_tone = 0.0f;
};

enum class Error {
    InvalidKana,
    OutOfMemory,
    Cancelled,  // synthesize_stream のシンクが false を返して中断した
};

const char* to_string(Error e);

tl::expected<void, Error> synthesize(std::u32string_view kana,
                                     std::vector<std::int16_t>& out,
                                     const Options& opt = {});

// synthesize() と同じだが、実際に出力した PCM のサンプルレートを返す。
// sanoTTS エンジンは 22.05 kHz 固定で、opt.sample_rate_hz を無視してこの
// レートで出力する (再生側がレートを合わせる)。他のエンジンは
// opt.sample_rate_hz のまま。synthesize() は Sano を「レートが一致するとき」
// だけ使う (一致しなければ次のエンジンへフォールバック)。
tl::expected<std::uint32_t, Error> synthesize_ex(std::u32string_view kana,
                                                 std::vector<std::int16_t>& out,
                                                 const Options& opt = {});

// リップシンク用の口形イベント。`start_ms` (発話先頭からの経過時間) から次の
// イベントまで、口は `vowel` の形を取る。Vowel::None は閉口。
struct VisemeEvent {
    std::uint32_t start_ms = 0;
    Vowel vowel = Vowel::None;
};

// synthesize に加えて、PCM と時間軸が揃った口形イベント列を `visemes` に返す
// (時刻昇順、隣り合うイベントの vowel は異なる。最後は閉口で終わる)。
// 口形を出せるのはフォルマント / HMM / sanoTTS (音素ごとの継続長から作る)。単位連結
// エンジンで合成された場合と失敗時は空になるので、呼び出し側は音量エンベロープなどに
// フォールバックすること。
tl::expected<void, Error> synthesize(std::u32string_view kana,
                                     std::vector<std::int16_t>& out,
                                     std::vector<VisemeEvent>& visemes,
                                     const Options& opt = {});

// ストリーミング合成の 1 チャンク分。
struct SynthChunk {
    // このチャンクが読む部分 (synthesize_stream に渡した読みの一部。HMM で強制分割した
    // ときはアクセント記号が落ちる)。発話全体が 1 チャンクなら読み全体。
    // 吹き出しをチャンクに同期させるとき (jtts/subtitle.hpp) に使う。
    std::u32string text;
    std::vector<std::int16_t> pcm;
    // pcm のサンプルレート [Hz]。HMM / フォルマント / 単位連結は opt.sample_rate_hz、
    // sanoTTS は 22.05 kHz 固定 (synthesize_ex と同じ)。再生側がこのレートで鳴らすこと。
    std::uint32_t sample_rate = 0;
    // このチャンク先頭からの口形イベント (synthesize と同じ規約)。空ならこの
    // エンジンは口形を出せない (音量エンベロープなどにフォールバックすること)。
    std::vector<VisemeEvent> visemes;
};

// チャンクの受け取り側。false を返すと合成を中断する (Error::Cancelled)。
using ChunkSink = std::function<bool(SynthChunk&&)>;

// synthesize と同じ合成を、チャンクごとに sink へ渡しながら行う。長い発話は
// HMM エンジンが句読点などで分割して順に合成するので、最初のチャンクが出来た
// 時点で再生を始め、再生中に次を合成できる (全体の合成完了を待たなくてよい)。
//   - チャンクの PCM は連続再生すればそのまま 1 本の発話になる (境界の無音は調整済み)。
//   - 最初のチャンクを小さく、以降を徐々に大きくして、再生が途切れにくくする。
//   - 単位連結 / フォルマントも sanoTTS と同じく句読点 (、。) ごとに 1 句ずつ合成して渡す。
//     句読点が無い短文は 1 チャンク。
//     句と句の間には HMM の pau と同じ長さの無音を明示的に挟む (句の後ろに足すので、
//     チャンクは「句 + 間」。話速に比例)。sanoTTS は synthesize_ex と同様に 22.05 kHz で
//     出力する (SynthChunk::sample_rate)。
//   - HMM で 1 つ以上渡した後にメモリ不足になったら Error::OutOfMemory (途中まで
//     渡した分は取り消せない)。何も渡す前なら他エンジンへフォールバックする。
// sink はこの関数を呼んだスレッドで、合成の合間に呼ばれる。
tl::expected<void, Error> synthesize_stream(std::u32string_view kana, const ChunkSink& sink,
                                            const Options& opt = {});

// 単位連結エンジン用の音声 DB (.jvox、codec=0 の生形式) を登録する。
// blob の寿命は呼び出し側が保証する (PSRAM バッファ / flash mmap)。
// パースに失敗すると false を返し、DB 未ロード状態のまま。空 span で解除。
// スレッド安全ではない — 発話中の差し替えは呼び出し側で直列化すること。
bool set_voice_db(std::span<const std::uint8_t> jvox_blob);

// ロード済み DB の単位数 (未ロードなら 0)。
std::uint16_t voice_db_units();

// HMM エンジン用の .htsvoice イメージを登録する。blob の寿命は呼び出し側が
// 保証する (flash mmap / PSRAM)。ロード完了後はパース済み構造がヒープに
// 展開されるが、再ロードに備えて blob は生かしておくこと。空 span で解除。
// パース失敗時は false (未ロード状態のまま)。スレッド安全ではない。
// CONFIG_JTTS_ENABLE_HMM 無効ビルドでは常に false。
bool set_hmm_voice(std::span<const std::uint8_t> htsvoice);

// HMM ボイスがロード済みか。
bool hmm_voice_loaded();

// sanoTTS-jp の重み blob (公式 Releases の saanotts-jp-v4-int8.bin、"SAAN" ヘッダ)
// を登録する。blob の寿命は呼び出し側が保証する (flash mmap / PSRAM)。コアは
// blob を直接参照するのでコピーしない。16 バイト境界に置くこと。空 span で解除。
// 検証失敗 (マジック/バージョン/形状) は false。スレッド安全ではない。
// CONFIG_JTTS_ENABLE_SANOTTS 無効ビルドでは常に false。
bool set_sano_weights(std::span<const std::uint8_t> blob);

// sanoTTS の重みがロード済みか。
bool sano_weights_loaded();

// sanoTTS の作業領域を呼び出し側のバッファに差し替える (16 バイト境界、176 KB 以上)。
// 既定はヒープ (Kconfig で内部 DRAM 優先 / PSRAM) から初回に確保する。公式構成と
// 同じ「.bss の静的配列」を使う検証用。合成中に呼ばないこと。
void set_sano_arena(void* buf, std::size_t size);

}  // namespace stackchan::jtts
