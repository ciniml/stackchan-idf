// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

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
enum class Engine : std::uint8_t {
    Auto = 0,
    Formant = 1,
    Unit = 2,
    Hmm = 3,
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
};

const char* to_string(Error e);

tl::expected<void, Error> synthesize(std::u32string_view kana,
                                     std::vector<std::int16_t>& out,
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

}  // namespace stackchan::jtts
