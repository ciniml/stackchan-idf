// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS の句ごとの合成 (split_clauses / trim_silence / stream_sano_clauses) の検証。
// 重み (非 MIT の blob) が無くても動くよう、1 句の合成は「前後に無音のある合成音」の
// スタブに差し替える。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"

extern "C" {
#include "g2p_table.h"  // 上流の仮名表 (kSaanG2pMora): 音素 ID の語彙と一致するか検証する
}

using namespace stackchan::jtts;
using namespace stackchan::jtts::internal;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

constexpr std::uint32_t kRate = 22050;

// ms → サンプル数
std::size_t samples(double ms) { return static_cast<std::size_t>(ms * kRate / 1000.0 + 0.5); }

// 前に lead_ms、後ろに trail_ms の無音がある、body_ms の音 (振幅 8000 の矩形波) を作る。
std::vector<std::int16_t> tone(double lead_ms, double body_ms, double trail_ms)
{
    std::vector<std::int16_t> v(samples(lead_ms), 0);
    for (std::size_t i = 0; i < samples(body_ms); ++i) v.push_back((i / 20) % 2 ? 8000 : -8000);
    v.insert(v.end(), samples(trail_ms), 0);
    return v;
}

// 先頭 / 末尾の無音のサンプル数 (絶対値 < 100 を無音とみなす)。
std::size_t lead_silence(const std::vector<std::int16_t>& v)
{
    std::size_t n = 0;
    while (n < v.size() && std::abs(v[n]) < 100) ++n;
    return n;
}
std::size_t trail_silence(const std::vector<std::int16_t>& v)
{
    std::size_t n = 0;
    while (n < v.size() && std::abs(v[v.size() - 1 - n]) < 100) ++n;
    return n;
}

void test_split_clauses()
{
    std::vector<HmmChunk> c;
    CHECK(!split_clauses(U"", c));
    CHECK(!split_clauses(U"、。", c));

    // 句読点の直後で切る。区切りは前の句に含め、続く句読点はまとめる。
    CHECK(split_clauses(U"あいう、えお。かき", c));
    CHECK(c.size() == 3);
    CHECK(c[0].text == U"あいう、" && c[0].moras == 3 && c[0].pause_after);
    CHECK(c[1].text == U"えお。" && c[1].moras == 2 && c[1].pause_after);
    CHECK(c[2].text == U"かき" && c[2].moras == 2 && !c[2].pause_after);

    CHECK(split_clauses(U"あ、、い。。。う", c));
    CHECK(c.size() == 3 && c[0].text == U"あ、、" && c[1].text == U"い。。。" && c[2].text == U"う");

    // 句読点が無ければ 1 句。アクセント記号 / アクセント句境界はそのまま句の中に残る。
    CHECK(split_clauses(U"こ'んにちは/ありが'とう", c));
    CHECK(c.size() == 1 && c[0].text == U"こ'んにちは/ありが'とう" && !c[0].pause_after);

    // 先頭の句読点は次の句の前に残り、末尾の記号は前の句に含まれる。全体の文字が失われない。
    CHECK(split_clauses(U"、あい、う。、", c));
    std::u32string joined;
    for (const auto& x : c) joined += x.text;
    CHECK(joined == U"、あい、う。、");
    CHECK(c.size() == 2 && c[0].text == U"、あい、" && c[1].text == U"う。、");
}

void test_trim_silence()
{
    auto v = tone(300, 200, 400);
    trim_silence(v, kRate, /*lead=*/true, /*trail=*/true, /*keep_ms=*/10);
    // 音の前後に 10 ms の余白だけ残す。
    CHECK(std::abs(static_cast<double>(lead_silence(v)) - static_cast<double>(samples(10))) <= 2);
    CHECK(std::abs(static_cast<double>(trail_silence(v)) - static_cast<double>(samples(10))) <= 2);
    CHECK(std::abs(static_cast<double>(v.size()) - static_cast<double>(samples(220))) <= 4);

    // 削った量が戻る (口形の切り詰めに使う)。
    {
        auto w = tone(300, 200, 400);
        const std::size_t before = w.size();
        const SilenceTrim cut = trim_silence(w, kRate, true, true, 10);
        CHECK(cut.front + cut.back + w.size() == before);
        CHECK(std::abs(static_cast<double>(cut.front) - static_cast<double>(samples(290))) <= 2);
        CHECK(std::abs(static_cast<double>(cut.back) - static_cast<double>(samples(390))) <= 2);
    }

    // lead / trail は独立。
    auto a = tone(300, 200, 400);
    trim_silence(a, kRate, true, false);
    CHECK(lead_silence(a) <= samples(10) + 2 && trail_silence(a) == samples(400));
    auto b = tone(300, 200, 400);
    trim_silence(b, kRate, false, true);
    CHECK(lead_silence(b) == samples(300) && trail_silence(b) <= samples(10) + 2);

    // 無音が余白より短ければ何もしない / 全体が無音なら触らない / 空も安全。
    auto c = tone(5, 100, 5);
    const auto c0 = c;
    trim_silence(c, kRate, true, true);
    CHECK(c == c0);
    std::vector<std::int16_t> z(1000, 0);
    trim_silence(z, kRate, true, true);
    CHECK(z.size() == 1000);
    std::vector<std::int16_t> e;
    trim_silence(e, kRate, true, true);
    CHECK(e.empty());

    // 小さな雑音は無音扱い (ピークの約 1 %)。
    auto n = tone(0, 100, 0);
    n.insert(n.begin(), 300, 20);
    n.insert(n.end(), 300, -20);
    trim_silence(n, kRate, true, true, 0);
    CHECK(n.size() == samples(100));
}

struct Got {
    std::vector<std::int16_t> pcm;
    std::uint32_t rate;
    std::vector<VisemeSpan> spans;
    std::u32string text;
};

// 口形の区間の合計 [ms]。
double total_ms(const std::vector<VisemeSpan>& sp)
{
    double t = 0;
    for (const auto& x : sp) t += x.duration_ms;
    return t;
}

void test_stream()
{
    Options opt;
    opt.mora_ms = 110.0f;  // 等速: 句間の無音は 420 ms

    // 各句は「前 300 ms / 音 200 ms / 後ろ 400 ms」を返すスタブ。
    int calls = 0;
    // 口形は「前 300 ms 閉口 / 音 200 ms は あ / 後ろ 400 ms 閉口」。
    const SanoClauseSynth synth = [&](const std::u32string&, std::vector<std::int16_t>& pcm, std::uint32_t& rate,
                                      std::vector<VisemeSpan>& spans) {
        ++calls;
        pcm = tone(300, 200, 400);
        rate = kRate;
        spans = {{Vowel::None, 300.0f}, {Vowel::A, 200.0f}, {Vowel::None, 400.0f}};
        return ClauseResult::Ok;
    };
    std::vector<Got> got;
    const SanoChunkFn collect = [&](std::vector<std::int16_t>&& pcm, std::uint32_t rate,
                                    std::vector<VisemeSpan>&& spans, const std::u32string& text) {
        got.push_back({std::move(pcm), rate, std::move(spans), text});
        return true;
    };

    CHECK(stream_sano_clauses(U"あいう、えお。かき", opt, synth, collect) == StreamOutcome::Ok);
    CHECK(calls == 3 && got.size() == 3);
    std::u32string joined;
    for (const auto& g : got) joined += g.text;
    CHECK(joined == U"あいう、えお。かき");  // 句の読みが欠けずに順に渡る
    for (const auto& g : got) CHECK(g.rate == kRate);

    // 先頭の句: 前の無音 (300 ms) は残し、後ろは切り詰めて 420 ms の無音に置き換える。
    CHECK(lead_silence(got[0].pcm) == samples(300));
    CHECK(std::abs(static_cast<double>(trail_silence(got[0].pcm)) - static_cast<double>(samples(420 + 10))) <= 4);
    // 中の句: 前後とも切り詰め (前は 10 ms の余白だけ)、後ろに 420 ms。
    CHECK(lead_silence(got[1].pcm) <= samples(10) + 2);
    CHECK(std::abs(static_cast<double>(trail_silence(got[1].pcm)) - static_cast<double>(samples(420 + 10))) <= 4);
    // 最後の句: 前は切り詰め、後ろは切り詰めない (従来どおり) & 無音も足さない。
    CHECK(lead_silence(got[2].pcm) <= samples(10) + 2);
    CHECK(trail_silence(got[2].pcm) == samples(400));
    // 句の間の実際の無音 (前の句の後ろ + 次の句の前) は HMM の pau に近い ≈ 0.42〜0.45 s。
    const double gap_ms = 1000.0 * static_cast<double>(trail_silence(got[0].pcm) + lead_silence(got[1].pcm)) / kRate;
    CHECK(gap_ms > 430 && gap_ms < 460);

    // 話速 (mora_ms) に比例して句間の無音が伸び縮みする (HMM の pau と同じ写像)。
    Options slow = opt;
    slow.mora_ms = 220.0f;
    got.clear();
    CHECK(stream_sano_clauses(U"あ、い", slow, synth, collect) == StreamOutcome::Ok);
    CHECK(std::abs(static_cast<double>(trail_silence(got[0].pcm)) - static_cast<double>(samples(840 + 10))) <= 4);
    Options fast = opt;
    fast.mora_ms = 55.0f;
    got.clear();
    CHECK(stream_sano_clauses(U"あ、い", fast, synth, collect) == StreamOutcome::Ok);
    CHECK(std::abs(static_cast<double>(trail_silence(got[0].pcm)) - static_cast<double>(samples(210 + 10))) <= 4);

    // 句が 1 つだけ (句読点なし) なら何も切り詰めず、無音も足さない = 従来どおりの 1 チャンク。
    got.clear();
    CHECK(stream_sano_clauses(U"こんにちは", opt, synth, collect) == StreamOutcome::Ok);
    CHECK(got.size() == 1 && got[0].pcm == tone(300, 200, 400));

    // 中断: emit が false を返したらそこで止まる。
    got.clear();
    calls = 0;
    int n = 0;
    const SanoChunkFn stop_after_first = [&](std::vector<std::int16_t>&&, std::uint32_t,
                                             std::vector<VisemeSpan>&&, const std::u32string&) { return ++n < 1; };
    CHECK(stream_sano_clauses(U"あ、い、う", opt, synth, stop_after_first) == StreamOutcome::Cancelled);
    CHECK(calls == 1);

    // 最初の句の失敗 = 何も出さず NoOutput (呼び出し側が他エンジンへ)。途中の失敗 = Aborted。
    const SanoClauseSynth fail_first = [&](const std::u32string&, std::vector<std::int16_t>&, std::uint32_t&,
                                           std::vector<VisemeSpan>&) { return ClauseResult::Fail; };
    got.clear();
    CHECK(stream_sano_clauses(U"あ、い", opt, fail_first, collect) == StreamOutcome::NoOutput && got.empty());
    int k = 0;
    const SanoClauseSynth fail_second = [&](const std::u32string& c, std::vector<std::int16_t>& pcm,
                                            std::uint32_t& rate, std::vector<VisemeSpan>& sp) {
        return ++k == 2 ? ClauseResult::Fail : synth(c, pcm, rate, sp);
    };
    got.clear();
    CHECK(stream_sano_clauses(U"あ、い、う", opt, fail_second, collect) == StreamOutcome::Aborted && got.size() == 1);

    // Skip (読める内容が無い句) は飛ばして続ける。全部 Skip なら NoOutput。
    k = 0;
    const SanoClauseSynth skip_second = [&](const std::u32string& c, std::vector<std::int16_t>& pcm,
                                            std::uint32_t& rate, std::vector<VisemeSpan>& sp) {
        return ++k == 2 ? ClauseResult::Skip : synth(c, pcm, rate, sp);
    };
    got.clear();
    CHECK(stream_sano_clauses(U"あ、い、う", opt, skip_second, collect) == StreamOutcome::Ok && got.size() == 2);
    const SanoClauseSynth skip_all = [&](const std::u32string&, std::vector<std::int16_t>&, std::uint32_t&,
                                         std::vector<VisemeSpan>&) { return ClauseResult::Skip; };
    got.clear();
    CHECK(stream_sano_clauses(U"あ、い", opt, skip_all, collect) == StreamOutcome::NoOutput && got.empty());

    // 読める内容が無い入力は NoOutput。
    CHECK(stream_sano_clauses(U"、。", opt, synth, collect) == StreamOutcome::NoOutput);

    // 重みが無い環境 (このテスト) では、本物の render_sano_stream は NoOutput でフォールバックさせる。
    got.clear();
    CHECK(render_sano_stream(U"あ、い", opt, collect) == StreamOutcome::NoOutput && got.empty());
    std::vector<std::int16_t> out;
    std::uint32_t rate = 0;
    CHECK(!render_sano(U"あ、い", out, opt, rate) && out.empty());
}

void test_crop_spans()
{
    std::vector<VisemeSpan> sp = {{Vowel::None, 100.0f}, {Vowel::A, 200.0f}, {Vowel::None, 300.0f}};
    auto a = sp;
    crop_spans(a, 0.0f, 600.0f);  // 何も削らない
    CHECK(a.size() == 3 && total_ms(a) == 600.0);
    a = sp;
    crop_spans(a, 150.0f, 100.0f);  // あ の途中から 100 ms
    CHECK(a.size() == 1 && a[0].vowel == Vowel::A && std::abs(a[0].duration_ms - 100.0f) < 1e-3);
    a = sp;
    crop_spans(a, 250.0f, 1000.0f);  // 先頭を落とし、後ろは残り全部
    CHECK(a.size() == 2 && a[0].vowel == Vowel::A && std::abs(a[0].duration_ms - 50.0f) < 1e-3 &&
          std::abs(total_ms(a) - 350.0) < 1e-3);
    a = sp;
    crop_spans(a, 50.0f, 300.0f);  // 閉口の一部 + あ + 閉口の一部
    CHECK(a.size() == 3 && std::abs(a[0].duration_ms - 50.0f) < 1e-3 && std::abs(a[2].duration_ms - 50.0f) < 1e-3);
}

// 音素 ID (saan_g2p の出力: ^ PAD 音素 PAD 音素 ... $) → 口形。1 フレーム = 10 ms として、d_hat を並べる。
std::vector<VisemeSpan> spans_of(const std::vector<int>& ids, const std::vector<int>& d)
{
    std::vector<VisemeSpan> out;
    std::vector<std::int32_t> i32(ids.begin(), ids.end()), d32(d.begin(), d.end());
    sano_ids_to_spans(i32.data(), d32.data(), static_cast<std::int32_t>(ids.size()), 10.0f, out);
    return out;
}

std::string shape(const std::vector<VisemeSpan>& sp)
{
    std::string s;
    for (const auto& x : sp) {
        const char* n = "-aiueo";
        s += n[static_cast<int>(x.vowel)];
    }
    return s;
}

void test_ids_to_spans()
{
    constexpr int PAD = 0, BOS = 1, EOS = 2, A = 10, I = 11, U = 12, E = 13, O = 14;
    constexpr int K = 25, M = 51, B = 37, N = 20, CL = 24, S = 41, Y = 56;

    // 「あいうえお」: ^ _ a _ i _ u _ e _ o _ $
    auto v = spans_of({BOS, PAD, A, PAD, I, PAD, U, PAD, E, PAD, O, PAD, EOS}, {2, 3, 10, 2, 10, 2, 10, 2, 10, 2, 10, 2, 5});
    CHECK(shape(v) == "-aiueo-");
    // 音素の後ろの PAD (余白) は直前の母音に含まれ、先頭の ^ _ と末尾の $ は閉口。
    CHECK(std::abs(v[0].duration_ms - 50.0f) < 1e-3);          // (2 + 3) × 10
    CHECK(std::abs(v[1].duration_ms - 120.0f) < 1e-3);         // a: 10 + 2 (余白)
    CHECK(std::abs(v.back().duration_ms - 50.0f) < 1e-3);      // 末尾の $ (5 フレーム)
    CHECK(std::abs(total_ms(v) - 10.0 * (2 + 3 + 10 + 2 + 10 + 2 + 10 + 2 + 10 + 2 + 10 + 2 + 5)) < 1e-3);

    // 子音は直後の母音の形を先取り (か: k a)。
    v = spans_of({BOS, PAD, K, PAD, A, PAD, EOS}, {2, 2, 5, 1, 10, 1, 4});
    CHECK(shape(v) == "-a-");
    CHECK(std::abs(v[1].duration_ms - 170.0f) < 1e-3);  // k 5 + PAD 1 + a 10 + PAD 1 = 17 フレーム

    // 両唇音 (ま: m a / ば: b a) は閉口 → 母音。
    v = spans_of({BOS, PAD, M, PAD, A, PAD, EOS}, {2, 2, 5, 1, 10, 1, 4});
    CHECK(shape(v) == "-a-");
    v = spans_of({BOS, PAD, B, PAD, I, PAD, EOS}, {2, 2, 5, 1, 10, 1, 4});
    CHECK(shape(v) == "-i-");
    CHECK(std::abs(v[0].duration_ms - (2 + 2 + 5 + 1) * 10.0f) < 1e-3);  // 両唇音 (と余白) までは閉口

    // 拗音 (きゃ = ky a) は a の形、よ (y o) は o。y + 母音。
    v = spans_of({BOS, PAD, Y, PAD, O, PAD, EOS}, {2, 2, 4, 1, 10, 1, 4});
    CHECK(shape(v) == "-o-");

    // ん / っ は閉口 (前後の母音の間で口が閉じる)。
    v = spans_of({BOS, PAD, A, PAD, N, PAD, A, PAD, EOS}, {2, 2, 8, 1, 6, 1, 8, 1, 4});
    CHECK(shape(v) == "-a-a-");
    v = spans_of({BOS, PAD, A, PAD, CL, PAD, K, PAD, A, PAD, EOS}, {2, 2, 8, 1, 6, 1, 4, 1, 8, 1, 4});
    CHECK(shape(v) == "-a-a-");

    // ポーズ (PAD が 2 つ続く 2 つ目以降) は閉口。
    v = spans_of({BOS, PAD, A, PAD, PAD, S, PAD, U, PAD, EOS}, {2, 2, 8, 1, 20, 6, 1, 8, 1, 4});
    CHECK(shape(v) == "-a-u-");
    CHECK(std::abs(v[2].duration_ms - 200.0f) < 1e-3);  // ポーズの 20 フレームだけが閉口

    // 無声化母音 (15..19) は閉口。
    v = spans_of({BOS, PAD, 15, PAD, EOS}, {2, 2, 8, 1, 4});
    CHECK(shape(v) == "-");

    // マーク [ ] # ? は余白として直前の形を保つ (a [ ] → a のまま)。
    v = spans_of({BOS, PAD, A, PAD, 8, PAD, 9, PAD, I, PAD, EOS}, {2, 2, 6, 1, 1, 1, 1, 1, 6, 1, 4});
    CHECK(shape(v) == "-ai-");
    CHECK(std::abs(v[1].duration_ms - (6 + 1 + 1 + 1 + 1 + 1) * 10.0f) < 1e-3);

    // 空 / null は空。
    CHECK(spans_of({}, {}).empty());
    std::vector<VisemeSpan> none;
    sano_ids_to_spans(nullptr, nullptr, 3, 10.0f, none);
    CHECK(none.empty());
}

// 口形の規則が使う音素 ID (母音 / ん / っ / 両唇音) が、上流の仮名表 (語彙) と一致する。
// 上流の語彙が変わると (g2p_table.h の更新) ここが落ちる。
void test_vocab_matches_upstream_table()
{
    const auto lookup = [](char32_t a, char32_t b = 0) -> const saan_g2p_mora* {
        const auto c1 = static_cast<std::uint8_t>(a - SAAN_G2P_KANA_BASE);
        const auto c2 = static_cast<std::uint8_t>(b ? b - SAAN_G2P_KANA_BASE : 0);
        for (const auto& m : kSaanG2pMora) {
            if (m.c1 == c1 && m.c2 == c2) return &m;
        }
        return nullptr;
    };
    // 母音: あいうえお = 10..14 (無声化 +5 = 15..19)。
    constexpr char32_t kVowels[] = {U'あ', U'い', U'う', U'え', U'お'};
    for (int i = 0; i < 5; ++i) {
        const auto* m = lookup(kVowels[i]);
        CHECK(m != nullptr && m->p0 == 10 + i && m->p1 < 0);
    }
    CHECK(SAAN_G2P_VOWEL_LO == 10 && SAAN_G2P_VOWEL_HI == 14 && SAAN_G2P_DEVOICE_STEP == 5);
    CHECK(SAAN_G2P_ID_PAD == 0 && SAAN_G2P_ID_BOS == 1 && SAAN_G2P_ID_EOS == 2);

    // 両唇音 (ま行 / ば行 / ぱ行、拗音を含む) の第 1 音素は、口形の規則で閉口 (35 36 37 38 51 52)。
    const std::vector<int> bilabial = {35, 36, 37, 38, 51, 52};
    const auto is_bilabial = [&](int id) { return std::find(bilabial.begin(), bilabial.end(), id) != bilabial.end(); };
    constexpr char32_t kBilabialKana[] = {U'ま', U'み', U'む', U'め', U'も', U'ば', U'び', U'ぶ', U'べ', U'ぼ',
                                          U'ぱ', U'ぴ', U'ぷ', U'ぺ', U'ぽ'};
    for (char32_t k : kBilabialKana) {
        const auto* m = lookup(k);
        CHECK(m != nullptr && is_bilabial(m->p0));
    }
    for (char32_t k : {U'み', U'び', U'ぴ'}) {
        for (char32_t y : {U'ゃ', U'ゅ', U'ょ'}) {
            const auto* m = lookup(k, y);
            CHECK(m != nullptr && is_bilabial(m->p0));
        }
    }
    // 逆に、両唇音でない子音 (か さ た な は ら わ や) は閉口扱いにならない。
    for (char32_t k : {U'か', U'さ', U'た', U'な', U'は', U'ら', U'わ', U'や', U'ふ', U'が', U'ざ', U'だ'}) {
        const auto* m = lookup(k);
        CHECK(m != nullptr && !is_bilabial(m->p0));
    }
    // っ = 24 (閉口)、ん = 20..23 の異音 (N_m = 20)。
    const auto* cl = lookup(U'っ');
    CHECK(cl != nullptr && cl->p0 == 24 && cl->p1 < 0);
    const auto* nn = lookup(U'ん');
    CHECK(nn != nullptr && nn->p0 == 20);
    // 「ん」の異音表 (後続の音素ごと) の値は全て 20..23 (口形の規則では ん = 閉口)。
    for (const auto id : kSaanG2pNAllophone) CHECK(id >= 20 && id <= 23);
}

}  // namespace

int main()
{
    test_split_clauses();
    test_trim_silence();
    test_crop_spans();
    test_ids_to_spans();
    test_vocab_matches_upstream_table();
    test_stream();
    if (g_failures == 0) std::puts("test_sano_clause: all passed");
    return g_failures == 0 ? 0 : 1;
}
