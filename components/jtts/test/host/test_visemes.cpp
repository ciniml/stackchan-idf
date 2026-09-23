// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 口形イベント (VisemeEvent) の検証。
//   jtts_test_visemes [voice.htsvoice ...]
// フォルマント エンジンのケースは常に実行する。.htsvoice を渡すと、それぞれを
// ロードして HMM エンジンのケースも実行する (例: assets/voices/mei16.htsvoice)。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "jtts/jtts.hpp"

using namespace stackchan::jtts;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

char to_char(Vowel v)
{
    switch (v) {
    case Vowel::A: return 'a';
    case Vowel::I: return 'i';
    case Vowel::U: return 'u';
    case Vowel::E: return 'e';
    case Vowel::O: return 'o';
    case Vowel::None: break;
    }
    return '-';
}

// 母音列を "aiueo-" のような文字列にする (None は '-')。
std::string shape_of(std::u32string_view kana, std::vector<std::int16_t>& pcm,
                     std::vector<VisemeEvent>& ev, const Options& opt)
{
    auto r = synthesize(kana, pcm, ev, opt);
    CHECK(r.has_value());
    std::string s;
    for (const auto& e : ev) s.push_back(to_char(e.vowel));
    return s;
}

// 母音 (None を除く) だけを並べた文字列。
std::string vowels_only(const std::vector<VisemeEvent>& ev)
{
    std::string s;
    for (const auto& e : ev) {
        if (e.vowel != Vowel::None) s.push_back(to_char(e.vowel));
    }
    return s;
}

// [from_ms, to_ms) の PCM の RMS。
double rms_between(const std::vector<std::int16_t>& pcm, std::uint32_t rate, double from_ms, double to_ms)
{
    const std::size_t a = static_cast<std::size_t>(from_ms * rate / 1000.0);
    const std::size_t b = std::min(pcm.size(), static_cast<std::size_t>(to_ms * rate / 1000.0));
    if (b <= a) return 0.0;
    double acc = 0.0;
    for (std::size_t i = a; i < b; ++i) acc += static_cast<double>(pcm[i]) * pcm[i];
    return std::sqrt(acc / static_cast<double>(b - a));
}

void test_hmm(const char* voice_path)
{
    std::ifstream f(voice_path, std::ios::binary);
    static std::vector<std::uint8_t> blob;  // set_hmm_voice は blob の寿命を要求する
    blob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    CHECK(!blob.empty());
    CHECK(set_hmm_voice(blob));
    std::printf("HMM voice: %s\n", voice_path);

    Options opt;
    opt.engine = Engine::Hmm;
    std::vector<std::int16_t> pcm;
    std::vector<VisemeEvent> ev;

    CHECK(synthesize(U"あいうえお", pcm, ev, opt).has_value());
    CHECK(!pcm.empty());
    CHECK(!ev.empty());
    CHECK(vowels_only(ev) == "aiueo");

    // 先頭は無音 (sil) で閉口、最後も閉口。時刻は昇順で隣接する母音は異なる。
    CHECK(ev.front().start_ms == 0 && ev.front().vowel == Vowel::None);
    CHECK(ev.back().vowel == Vowel::None);
    for (std::size_t i = 1; i < ev.size(); ++i) {
        CHECK(ev[i].start_ms > ev[i - 1].start_ms);
        CHECK(ev[i].vowel != ev[i - 1].vowel);
    }
    const double pcm_ms = 1000.0 * static_cast<double>(pcm.size()) / opt.sample_rate_hz;
    CHECK(static_cast<double>(ev.back().start_ms) <= pcm_ms + 1.0);
    CHECK(pcm_ms - static_cast<double>(ev.back().start_ms) < 1000.0);

    // PCM との時間軸の一致: 先頭の無音区間は静かで、最初の母音区間は音が出ている。
    for (std::size_t i = 0; i + 1 < ev.size(); ++i) {
        if (ev[i].vowel == Vowel::None) continue;
        const double v0 = ev[i].start_ms, v1 = ev[i + 1].start_ms;
        const double sil = rms_between(pcm, opt.sample_rate_hz, 0.0, ev.front().start_ms + 0.5 * (ev[1].start_ms));
        const double voiced = rms_between(pcm, opt.sample_rate_hz, v0 + 0.25 * (v1 - v0), v0 + 0.75 * (v1 - v0));
        std::printf("  first vowel @%.0f-%.0f ms: rms(sil)=%.1f rms(vowel)=%.1f\n", v0, v1, sil, voiced);
        CHECK(voiced > 4.0 * sil + 100.0);
        break;
    }

    // 無声化母音は閉口: 「きした」で母音は た の a だけ。
    CHECK(synthesize(U"きした", pcm, ev, opt).has_value());
    CHECK(vowels_only(ev) == "a");

    // 呼気段落境界の pau は閉口として挟まる。
    CHECK(synthesize(U"あ、い", pcm, ev, opt).has_value());
    CHECK(vowels_only(ev) == "ai");

    // 話速 (mora_ms) を倍にすると口形の時間軸も約 2 倍に伸びる。
    CHECK(synthesize(U"あいうえお", pcm, ev, opt).has_value());
    const double t_normal = ev.back().start_ms;
    Options slow = opt;
    slow.mora_ms = 220.0f;
    CHECK(synthesize(U"あいうえお", pcm, ev, slow).has_value());
    CHECK(vowels_only(ev) == "aiueo");
    CHECK(ev.back().start_ms > 1.5 * t_normal);

    // Auto でも HMM が選ばれ口形が出る。
    Options autoo;
    CHECK(synthesize(U"あいうえお", pcm, ev, autoo).has_value());
    CHECK(vowels_only(ev) == "aiueo");

    set_hmm_voice({});
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    opt.engine = Engine::Formant;  // 口形を出せるのはフォルマントのみ

    std::vector<std::int16_t> pcm;
    std::vector<VisemeEvent> ev;

    // 母音そのまま: 母音ごとに 1 イベント + 終端の閉口。
    CHECK(shape_of(U"あいうえお", pcm, ev, opt) == "aiueo-");

    // 時刻は昇順、隣接イベントの母音は異なり、終端は PCM 長と一致する。
    for (std::size_t i = 1; i < ev.size(); ++i) {
        CHECK(ev[i].start_ms > ev[i - 1].start_ms);
        CHECK(ev[i].vowel != ev[i - 1].vowel);
    }
    const double pcm_ms = 1000.0 * static_cast<double>(pcm.size()) / opt.sample_rate_hz;
    CHECK(std::abs(static_cast<double>(ev.back().start_ms) - pcm_ms) < 5.0);
    CHECK(ev.front().start_ms == 0);

    // 両唇音は閉口 → 母音。それ以外の子音は母音の形を先取りする。
    CHECK(shape_of(U"ま", pcm, ev, opt) == "-a-");
    CHECK(shape_of(U"か", pcm, ev, opt) == "a-");
    CHECK(ev.front().start_ms == 0);

    // 促音・撥音は閉口、長音は直前の母音を保つ。
    CHECK(shape_of(U"あっあ", pcm, ev, opt) == "a-a-");
    CHECK(shape_of(U"あんあ", pcm, ev, opt) == "a-a-");
    CHECK(shape_of(U"あーあ", pcm, ev, opt) == "a-");

    // 無声化母音は閉口のまま。「きした」は き・し とも無声化される。
    CHECK(shape_of(U"きした", pcm, ev, opt) == "-a-");
    CHECK(shape_of(U"ひとつ", pcm, ev, opt) == "-o-");

    // 空 / 不正な入力ではイベントも空。
    auto bad = synthesize(U"", pcm, ev, opt);
    CHECK(!bad.has_value());
    CHECK(ev.empty());

    // 既存の 3 引数 API と PCM が一致する (口形の収集が合成を変えない)。
    std::vector<std::int16_t> pcm_plain;
    CHECK(synthesize(U"こんにちは", pcm_plain, opt).has_value());
    CHECK(synthesize(U"こんにちは", pcm, ev, opt).has_value());
    CHECK(pcm == pcm_plain);

    for (int i = 1; i < argc; ++i) test_hmm(argv[i]);

    if (g_failures == 0) std::puts("test_visemes: all passed");
    return g_failures == 0 ? 0 : 1;
}
