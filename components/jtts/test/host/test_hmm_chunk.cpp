// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HMM 合成の長文分割 (split_hmm_text / 分割合成 / メモリ不足フォールバック) の検証。
//   jtts_test_hmm_chunk [voice.htsvoice ...]
// 分割ロジックは常に検証する。.htsvoice を渡すと、それぞれをロードして、メモリ予算を
// 絞った分割合成も検証する (例: assets/voices/mei16.htsvoice)。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"
#include "jtts/subtitle.hpp"

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

std::u32string join(const std::vector<internal::HmmChunk>& chunks)
{
    std::u32string s;
    for (const auto& c : chunks) s += c.text;
    return s;
}

std::u32string strip_accent(std::u32string s)
{
    s.erase(std::remove(s.begin(), s.end(), U'\''), s.end());
    return s;
}

void test_split()
{
    using internal::HmmChunk;
    using internal::split_hmm_text;
    std::vector<HmmChunk> ch;

    // 収まるなら手を加えず 1 チャンク (アクセント記号もそのまま)。
    CHECK(split_hmm_text(U"こ'んにちは", 100, ch));
    CHECK(ch.size() == 1 && ch[0].text == U"こ'んにちは" && ch[0].moras == 5);

    // 不正入力。
    CHECK(!split_hmm_text(U"こんにちは", 0, ch));
    CHECK(!split_hmm_text(U"、。", 10, ch));
    CHECK(!split_hmm_text(U"", 10, ch));

    // 句読点で分け、予算に収まる範囲で貪欲に詰める。区切りは前のチャンクに残る。
    CHECK(split_hmm_text(U"あいう、えお。かき", 5, ch));
    CHECK(ch.size() == 2);
    CHECK(ch[0].text == U"あいう、えお。" && ch[0].moras == 5 && ch[0].pause_after);
    CHECK(ch[1].text == U"かき" && ch[1].moras == 2 && !ch[1].pause_after);
    CHECK(join(ch) == U"あいう、えお。かき");

    CHECK(split_hmm_text(U"あいう、えお。かき", 3, ch));
    CHECK(ch.size() == 3);
    CHECK(ch[0].text == U"あいう、" && ch[0].pause_after);
    CHECK(ch[1].text == U"えお。" && ch[1].pause_after);
    CHECK(ch[2].text == U"かき");

    // アクセント句境界 (/) ではポーズなし。
    CHECK(split_hmm_text(U"あいう/えお", 3, ch));
    CHECK(ch.size() == 2 && ch[0].text == U"あいう/" && !ch[0].pause_after && ch[1].text == U"えお");

    // 句読点だけの句は前後に吸収され、モーラを持たないチャンクは作らない。
    CHECK(split_hmm_text(U"あい、、、うえ", 2, ch));
    for (const auto& c : ch) CHECK(c.moras > 0 && c.moras <= 2);
    CHECK(join(ch) == U"あい、、、うえ");

    // 句が単体で予算を超えるとモーラ境界で強制分割する。各チャンクは予算内。
    const std::u32string flat = U"あいうえおかきくけこさしすせそたちつてとなにぬねの";
    CHECK(split_hmm_text(flat, 8, ch));
    CHECK(ch.size() == 4);
    for (const auto& c : ch) CHECK(c.moras <= 8 && c.moras > 0);
    CHECK(join(ch) == flat);

    // 拗音・長音・アクセント核の直前では切らない。強制分割ではアクセント核を落とす。
    const std::u32string youon = U"きゃきゅきょきゃきゅきょきゃきゅきょ";
    CHECK(split_hmm_text(youon, 4, ch));
    for (const auto& c : ch) {
        CHECK(c.moras <= 4);
        CHECK(c.text.front() != U'ゃ' && c.text.front() != U'ゅ' && c.text.front() != U'ょ');
    }
    CHECK(join(ch) == youon);
    CHECK(split_hmm_text(U"あ'いう'えおかきくけこ", 4, ch));
    for (const auto& c : ch) CHECK(c.text.find(U'\'') == std::u32string::npos);
    CHECK(join(ch) == strip_accent(U"あ'いう'えおかきくけこ"));

    // 実際の長文 (146 文字) が予算 20 モーラで全て収まる。
    const std::u32string longtext =
        U"すたっくちゃんは、ちいさくてかわいい、てのひらさいずのろぼっとです。"
        U"かおのひょうじをかえたり、くびをうごかしたり、おしゃべりしたりできます。"
        U"じぶんでぷろぐらむをつくって、いろいろなことをさせられるのも、たのしいところです。"
        U"つくるひとによって、いろいろなこせいがうまれる、たのしいろぼっとです。";
    CHECK(split_hmm_text(longtext, 20, ch));
    CHECK(ch.size() > 4);
    for (const auto& c : ch) CHECK(c.moras <= 20 && c.moras > 0);
    CHECK(join(ch) == longtext);
    // 句読点で終わるチャンクが大半 (強制分割は不要な長さの句ばかり)。
    std::size_t pauses = 0;
    for (const auto& c : ch) pauses += c.pause_after ? 1 : 0;
    CHECK(pauses + 1 >= ch.size());

    // 低遅延モード (first_moras > 0): 先頭を小さく出し、句の途中では切らない。
    CHECK(split_hmm_text(longtext, 23, ch, 14));
    CHECK(join(ch) == longtext);
    CHECK(ch.front().moras <= 14);
    CHECK(ch.size() >= 8);
    for (std::size_t i = 0; i < ch.size(); ++i) {
        CHECK(ch[i].moras > 0 && ch[i].moras <= 23);
        CHECK(ch[i].pause_after);  // 全て句読点で終わる = 句の途中では切っていない
        // 直前のチャンクの 1.3 倍を超えて急に大きくならない (先頭 14 モーラ以内は除く)。
        if (i > 0) CHECK(ch[i].moras <= std::max<std::size_t>(14, (ch[i - 1].moras * 13 + 9) / 10) ||
                         ch[i].moras <= ch[i - 1].moras + 4);
    }
    // 句読点の無い短文は 1 チャンクのまま (原文どおり)。
    CHECK(split_hmm_text(U"こんにちは、", 23, ch, 14));
    CHECK(ch.size() == 1 && ch[0].text == U"こんにちは、");
    CHECK(split_hmm_text(U"こ'んにちはありが'とうございま'す", 23, ch, 14));
    CHECK(ch.size() == 1 && ch[0].text == U"こ'んにちはありが'とうございま'す");
    // 全体が予算内でも、句読点があれば先頭を早く出すために分ける。
    CHECK(split_hmm_text(U"あいうえお、かきくけこさし、たちつてと", 23, ch, 14));
    CHECK(ch.size() == 2 && ch[0].moras == 12 && ch[1].moras == 5);
    // 非低遅延 (first_moras = 0) では従来どおり 1 チャンク。
    CHECK(split_hmm_text(U"あいうえお、かきくけこさし、たちつてと", 23, ch));
    CHECK(ch.size() == 1);
}

// 吹き出しをチャンクに対応させる。
void test_subtitle()
{
    // 句読点ごとに対応する。
    {
        SubtitleMapper m("スタックチャンは、小さくて、かわいい。ロボットです。",
                         U"すたっくちゃんは、ちいさくて、かわいい。ろぼっとです。");
        CHECK(m.mapped());
        CHECK(m.next(U"すたっくちゃんは、") == "スタックチャンは、");
        CHECK(m.next(U"ちいさくて、かわいい。") == "小さくて、かわいい。");
        CHECK(m.next(U"ろぼっとです。") == "ロボットです。");
    }
    // 1 チャンクが複数の句を受け持つ。
    {
        SubtitleMapper m("A、B、C。", U"あ、い、う。");
        CHECK(m.mapped());
        CHECK(m.next(U"あ、い、") == "A、B、");
        CHECK(m.next(U"う。") == "C。");
    }
    // 句の途中で切れたチャンク (強制分割) は、同じ句を続けて返す。
    {
        SubtitleMapper m("あいうえおかきくけこ、さしすせそ。", U"あいうえおかきくけこ、さしすせそ。");
        CHECK(m.next(U"あいうえお") == "あいうえおかきくけこ、");
        CHECK(m.next(U"かきくけこ、") == "あいうえおかきくけこ、");
        CHECK(m.next(U"さしすせそ。") == "さしすせそ。");
    }
    // アクセント記号・空白が付いた読み。
    {
        SubtitleMapper m("今日は。天気。", U"きょ'うは。 てんき。");
        CHECK(m.next(U"きょ'うは。 ") == "今日は。");
        CHECK(m.next(U"てんき。") == "天気。");
    }
    // 句読点が無ければ全体を 1 つの句として返す。
    {
        SubtitleMapper m("こんにちは", U"こんにちは");
        CHECK(m.mapped() && m.next(U"こんにちは") == "こんにちは");
    }
    // 対応が取れない (句読点の数が違う / 表示が空): 最初に全文、以降は空。
    for (const auto* d : {"こんにちは！げんき？", ""}) {
        SubtitleMapper m(d, U"こんにちは、げんき？");
        CHECK(!m.mapped());
        CHECK(m.next(U"こんにちは、") == d);
        CHECK(m.next(U"げんき？").empty());
    }
}

std::string vowels_only(const std::vector<VisemeEvent>& ev)
{
    std::string s;
    for (const auto& e : ev) {
        switch (e.vowel) {
        case Vowel::A: s.push_back('a'); break;
        case Vowel::I: s.push_back('i'); break;
        case Vowel::U: s.push_back('u'); break;
        case Vowel::E: s.push_back('e'); break;
        case Vowel::O: s.push_back('o'); break;
        case Vowel::None: break;
        }
    }
    return s;
}

// 母音列の編集距離 (Levenshtein)。
std::size_t edit_distance(const std::string& a, const std::string& b)
{
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1] ? 1u : 0u)});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// synthesize_stream: チャンクごとに渡され、最初のチャンクが早く出て、連結すると
// 一括合成と同じ発話になる。
void test_stream(const std::u32string& longtext, const Options& opt, const std::string& ref_vowels, double ref_ms)
{
    internal::set_hmm_memory_budget_for_test(0);
    std::vector<SynthChunk> got;
    auto r = synthesize_stream(
        longtext,
        [&](SynthChunk&& c) {
            got.push_back(std::move(c));
            return true;
        },
        opt);
    CHECK(r.has_value());
    std::printf("  stream: %zu chunks", got.size());
    CHECK(got.size() >= 6);

    // 連結: PCM の長さと母音列 (チャンク先頭からの口形を積算して並べ直す)。
    std::size_t total_samples = 0;
    std::string vowels;
    for (const auto& c : got) {
        CHECK(!c.pcm.empty());
        total_samples += c.pcm.size();
        CHECK(!c.visemes.empty() && c.visemes.front().start_ms == 0);
        CHECK(c.visemes.back().vowel == Vowel::None);
        for (std::size_t i = 1; i < c.visemes.size(); ++i) CHECK(c.visemes[i].start_ms > c.visemes[i - 1].start_ms);
        vowels += vowels_only(c.visemes);
    }
    const double ms = 1000.0 * static_cast<double>(total_samples) / opt.sample_rate_hz;
    const double first_ms = 1000.0 * static_cast<double>(got.front().pcm.size()) / opt.sample_rate_hz;
    std::printf(", total %.0f ms (%+.1f%%), first chunk %.0f ms\n", ms, (ms - ref_ms) * 100.0 / ref_ms, first_ms);
    CHECK(vowels == ref_vowels);
    CHECK(std::abs(ms - ref_ms) < 0.05 * ref_ms);
    CHECK(first_ms < 0.2 * ms && first_ms < 3000.0);  // 低遅延: 先頭は全体の 1/5 未満・3 秒未満
    // 各チャンクの口形は自分の PCM の長さに収まる。
    for (const auto& c : got) {
        const double len = 1000.0 * static_cast<double>(c.pcm.size()) / opt.sample_rate_hz;
        CHECK(static_cast<double>(c.visemes.back().start_ms) <= len + 1.0);
    }

    // 各チャンクの読み (text) を連結すると元の読みになり、それに合わせて表示テキストを
    // 切り出すと、全チャンクで空でなく、連結すると表示テキスト全体になる。
    {
        const std::string display =
            "スタックチャンは、小さくてかわいい、手のひらサイズのロボットです。"
            "顔の表情を変えたり、首を動かしたり、おしゃべりしたりできます。"
            "自分でプログラムを作って、いろいろなことをさせられるのも、楽しいところです。"
            "作る人によって、いろいろな個性が生まれる、楽しいロボットです。";
        SubtitleMapper m(display, longtext);
        CHECK(m.mapped());
        std::u32string joined;
        std::string shown;
        for (const auto& c : got) {
            joined += c.text;
            const std::string t = m.next(c.text);
            CHECK(!t.empty());
            shown += t;
        }
        CHECK(joined == longtext);
        CHECK(shown == display);
    }

    // sink が false を返すと中断: それ以降のチャンクは合成されない。
    int calls = 0;
    r = synthesize_stream(
        longtext,
        [&](SynthChunk&&) { return ++calls < 2; },
        opt);
    CHECK(!r.has_value() && r.error() == Error::Cancelled);
    CHECK(calls == 2);

    // 全体の PCM が収まらない空き (400 KB) でも、HMM はチャンクごとなので発話できる。
    internal::set_pcm_memory_limit_for_test(400 * 1024);
    std::size_t n = 0;
    r = synthesize_stream(
        longtext,
        [&](SynthChunk&& c) {
            ++n;
            CHECK(c.pcm.size() * 2 < 400 * 1024);  // 1 チャンクなら十分小さい
            return true;
        },
        opt);
    CHECK(r.has_value() && n >= 6);
    // 一括版は同じ条件だと断る (全体を 1 本の PCM に持つため)。
    std::vector<std::int16_t> whole;
    auto rw = synthesize(longtext, whole, opt);
    CHECK(!rw.has_value() && rw.error() == Error::OutOfMemory);
    internal::set_pcm_memory_limit_for_test(0);

    // 予算を絞っても (チャンクがさらに小さくなるだけで) 最後まで発話できる。
    internal::set_hmm_memory_budget_for_test(std::size_t{1500} * 1024);
    n = 0;
    r = synthesize_stream(
        longtext,
        [&](SynthChunk&&) {
            ++n;
            return true;
        },
        opt);
    CHECK(r.has_value() && n >= 6);

    // 1 モーラも収まらない予算: 何も渡す前なので他エンジン (フォルマント) にフォールバック (句ごとのチャンク)。
    internal::set_hmm_memory_budget_for_test(64 * 1024);
    got.clear();
    r = synthesize_stream(
        longtext,
        [&](SynthChunk&& c) {
            got.push_back(std::move(c));
            return true;
        },
        opt);
    CHECK(r.has_value() && got.size() == 12 && !got[0].pcm.empty());
    internal::set_hmm_memory_budget_for_test(0);
}

void test_hmm(const char* voice_path)
{
    std::ifstream f(voice_path, std::ios::binary);
    static std::vector<std::uint8_t> blob;  // set_hmm_voice は blob の寿命を要求する
    blob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    CHECK(set_hmm_voice(blob));
    std::printf("HMM voice: %s\n", voice_path);

    Options opt;
    opt.engine = Engine::Hmm;
    opt.mora_ms = 120.0f;  // 実機の既定
    const std::u32string longtext =
        U"すたっくちゃんは、ちいさくてかわいい、てのひらさいずのろぼっとです。"
        U"かおのひょうじをかえたり、くびをうごかしたり、おしゃべりしたりできます。"
        U"じぶんでぷろぐらむをつくって、いろいろなことをさせられるのも、たのしいところです。"
        U"つくるひとによって、いろいろなこせいがうまれる、たのしいろぼっとです。";

    // 無制限 (1 チャンク) の基準。
    internal::set_hmm_memory_budget_for_test(0);
    std::vector<std::int16_t> ref_pcm;
    std::vector<VisemeEvent> ref_ev;
    CHECK(synthesize(longtext, ref_pcm, ref_ev, opt).has_value());
    const std::string ref_vowels = vowels_only(ref_ev);
    const double ref_ms = 1000.0 * static_cast<double>(ref_pcm.size()) / opt.sample_rate_hz;
    std::printf("  unchunked: %.0f ms, %zu vowels\n", ref_ms, ref_vowels.size());
    CHECK(!ref_vowels.empty());

    // 予算を絞って分割合成: 落ちずに合成でき、母音列も長さも基準に近い。
    // 実機で想定する予算 (2 MB 前後) では句読点でだけ分かれ、母音列は完全に一致する。
    // 極端に小さい予算 (句の途中で強制分割される) では、切れ目で無声化の文脈が
    // 失われて母音が数個ずれるのを許容する。
    struct Case {
        std::size_t budget_kb;
        std::size_t max_edit;  // 母音列の許容編集距離
        double tol;            // 長さの許容誤差 (割合)
    };
    for (const Case c : {Case{2000, 0, 0.05}, Case{1500, 1, 0.05}, Case{1000, 4, 0.20}}) {
        internal::set_hmm_memory_budget_for_test(c.budget_kb * 1024);
        std::vector<std::int16_t> pcm;
        std::vector<VisemeEvent> ev;
        CHECK(synthesize(longtext, pcm, ev, opt).has_value());
        const double ms = 1000.0 * static_cast<double>(pcm.size()) / opt.sample_rate_hz;
        const std::size_t dist = edit_distance(vowels_only(ev), ref_vowels);
        std::printf("  budget %zu KB: %.0f ms (%+.1f%%), vowel edit distance %zu\n", c.budget_kb, ms,
                    (ms - ref_ms) * 100.0 / ref_ms, dist);
        CHECK(dist <= c.max_edit);
        CHECK(std::abs(ms - ref_ms) < c.tol * ref_ms);

        // 口形イベントは PCM と時間軸が揃う: 昇順、隣接は異なる、最後は PCM 長以内で閉口。
        CHECK(!ev.empty() && ev.front().start_ms == 0);
        for (std::size_t i = 1; i < ev.size(); ++i) {
            CHECK(ev[i].start_ms > ev[i - 1].start_ms);
            CHECK(ev[i].vowel != ev[i - 1].vowel);
        }
        CHECK(ev.back().vowel == Vowel::None);
        CHECK(static_cast<double>(ev.back().start_ms) <= ms + 1.0);
        CHECK(ms - static_cast<double>(ev.back().start_ms) < 1000.0);
    }

    // 母音の位置と実際の音: 分割合成でも各母音区間に音が出ている (無音のまま
    // 母音イベントが立っていない)。全母音区間の RMS が背景 (先頭 sil) を上回る割合を見る。
    {
        internal::set_hmm_memory_budget_for_test(std::size_t{800} * 1024);
        std::vector<std::int16_t> pcm;
        std::vector<VisemeEvent> ev;
        CHECK(synthesize(longtext, pcm, ev, opt).has_value());
        std::size_t voiced = 0, total = 0;
        for (std::size_t i = 0; i + 1 < ev.size(); ++i) {
            if (ev[i].vowel == Vowel::None) continue;
            const double len = ev[i + 1].start_ms - ev[i].start_ms;
            if (len < 40.0) continue;
            const std::size_t a = static_cast<std::size_t>((ev[i].start_ms + 0.25 * len) * 16.0);
            const std::size_t b = std::min(pcm.size(), static_cast<std::size_t>((ev[i].start_ms + 0.75 * len) * 16.0));
            double acc = 0;
            for (std::size_t k = a; k < b; ++k) acc += static_cast<double>(pcm[k]) * pcm[k];
            const double rms = std::sqrt(acc / static_cast<double>(std::max<std::size_t>(1, b - a)));
            ++total;
            if (rms > 300.0) ++voiced;
        }
        std::printf("  vowel spans with sound: %zu / %zu\n", voiced, total);
        CHECK(total > 20 && voiced * 10 >= total * 9);
    }

    test_stream(longtext, opt, ref_vowels, ref_ms);

    // 1 モーラも収まらない予算: HMM を諦めて (クラッシュせず) フォールバックする。
    internal::set_hmm_memory_budget_for_test(64 * 1024);
    {
        std::vector<std::int16_t> pcm;
        std::vector<VisemeEvent> ev;
        CHECK(synthesize(longtext, pcm, ev, opt).has_value());
        CHECK(!pcm.empty());  // フォルマント合成で音が出る
    }
    internal::set_hmm_memory_budget_for_test(0);
    set_hmm_voice({});
}

// 発話が長すぎて PCM が空きメモリに収まらないときは、どのエンジンでも合成せず
// OutOfMemory を返す (確保失敗によるクラッシュを避ける)。
void test_pcm_limit()
{
    Options opt;
    opt.engine = Engine::Formant;
    std::vector<std::int16_t> pcm;
    std::vector<VisemeEvent> ev;
    const std::u32string longtext =
        U"すたっくちゃんは、ちいさくてかわいい、てのひらさいずのろぼっとです。"
        U"かおのひょうじをかえたり、くびをうごかしたり、おしゃべりしたりできます。"
        U"じぶんでぷろぐらむをつくって、いろいろなことをさせられるのも、たのしいところです。"
        U"つくるひとによって、いろいろなこせいがうまれる、たのしいろぼっとです。";

    // 空きが 400 KB: 短い発話は通り、長文 (PCM ≈ 1 MB) は断る。
    internal::set_pcm_memory_limit_for_test(400 * 1024);
    CHECK(synthesize(U"こんにちは", pcm, ev, opt).has_value());
    CHECK(!pcm.empty());
    auto r = synthesize(longtext, pcm, ev, opt);
    CHECK(!r.has_value() && r.error() == Error::OutOfMemory);
    CHECK(pcm.empty() && ev.empty());
    // 3 引数版も同じ。
    r = synthesize(longtext, pcm, opt);
    CHECK(!r.has_value() && r.error() == Error::OutOfMemory);

    // ストリーミングは句 (、。) ごとに 1 チャンク (この長文は 3 句 × 4 文 = 12) ずつ渡すので、
    // 長文でも 1 句が収まれば通る (一括版は全体を 1 本にするので断られる)。
    std::size_t chunks = 0;
    std::size_t max_chunk_bytes = 0;
    auto rs = synthesize_stream(
        longtext,
        [&](SynthChunk&& c) {
            ++chunks;
            max_chunk_bytes = std::max(max_chunk_bytes, c.pcm.size() * sizeof(std::int16_t));
            CHECK(!c.pcm.empty() && !c.visemes.empty() && c.visemes.front().start_ms == 0);
            return true;
        },
        opt);
    CHECK(rs.has_value() && chunks == 12 && max_chunk_bytes < 400 * 1024);

    // 上限なし (ホスト既定) なら長文も一括で合成できる。
    internal::set_pcm_memory_limit_for_test(0);
    CHECK(synthesize(longtext, pcm, ev, opt).has_value());
    CHECK(pcm.size() > 16000 * 10);  // 10 秒以上 (500 KB 超: 上の 400 KB 制限では収まらない長さ)
}

}  // namespace

int main(int argc, char** argv)
{
    test_split();
    test_subtitle();
    test_pcm_limit();
    for (int i = 1; i < argc; ++i) test_hmm(argv[i]);
    if (g_failures == 0) std::puts("test_hmm_chunk: all passed");
    return g_failures == 0 ? 0 : 1;
}
