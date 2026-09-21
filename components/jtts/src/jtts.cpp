// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "jtts/jtts.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "internal.hpp"
#include "jtts/jvox.hpp"

namespace stackchan::jtts {

namespace {
// 単位連結エンジンの音声 DB。blob 自体の寿命は set_voice_db の呼び出し側が
// 持つ (差し替え時は旧 blob を 1 世代残すこと)。Db ビューは shared_ptr の
// atomic 差し替えにして、合成中のタスクと HTTP アップロードの競合で
// ぶら下がりポインタを踏まないようにする。
std::atomic<std::shared_ptr<const jvox::Db>> g_voice_db;
}  // namespace

bool set_voice_db(std::span<const std::uint8_t> jvox_blob)
{
    if (jvox_blob.empty()) {
        g_voice_db.store(nullptr);
        return true;
    }
    auto db = jvox::Db::parse(jvox_blob);
    if (!db) return false;
    g_voice_db.store(std::make_shared<const jvox::Db>(*db));
    return true;
}

std::uint16_t voice_db_units()
{
    auto db = g_voice_db.load();
    return db ? db->unit_count() : 0;
}

const char* to_string(Error e) {
    switch (e) {
        case Error::InvalidKana: return "InvalidKana";
        case Error::OutOfMemory: return "OutOfMemory";
        case Error::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

namespace {

Options resolve_defaults(Options opt) {
    if (opt.f0_hz <= 0.0f) {
        opt.f0_hz = (opt.voice == Voice::Female) ? 210.0f : 130.0f;
    }
    if (opt.formant_scale <= 0.0f) {
        opt.formant_scale = (opt.voice == Voice::Female) ? 1.17f : 1.0f;
    }
    return opt;
}

void apply_formant_scale(std::vector<internal::Segment>& segs, float scale) {
    if (scale == 1.0f) return;
    auto scale_frame = [scale](internal::FormantFrame& f) {
        f.f1 *= scale;
        f.f2 *= scale;
        f.f3 *= scale;
        f.bw1 *= scale;
        f.bw2 *= scale;
        f.bw3 *= scale;
        // 鼻音ゼロも声道長に追従させる (V2 の鼻音極は render 側で同率スケール
        // されるため、ここで揃えないと nasal=0 の打ち消しが崩れる)。
        f.nasal_zero_hz *= scale;
    };
    for (auto& s : segs) {
        scale_frame(s.start);
        scale_frame(s.end);
    }
}

// セグメント列 (時間軸は PCM と一致) を口形イベント列にまとめる。
void collect_visemes(std::span<const internal::Segment> segs, std::vector<VisemeEvent>& out) {
    internal::VisemeBuilder b(out);
    for (const auto& s : segs) b.add(s.vowel, s.duration_ms);
    b.finish();
}

// sanoTTS: Auto では最優先、Sano 指定では必須。出力は 22.05 kHz 固定なので、呼び出し側が
// レートを受け取れる (synthesize_ex / synthesize_stream) か、要求レートが一致するとき
// だけ使う。
bool wants_sano(const Options& opt) {
    return opt.engine == Engine::Auto || opt.engine == Engine::Sano;
}

// HMM: Auto / Hmm、および Sano 指定 (重み未ロード時のフォールバック先)。
bool wants_hmm(const Options& opt) {
    return opt.engine == Engine::Auto || opt.engine == Engine::Hmm || opt.engine == Engine::Sano;
}

// HMM 以外のエンジン (単位連結 → フォルマント) で発話全体を 1 本の PCM にする。
// 全体を 1 つの std::vector に作るので、空きメモリに収まらない長さは合成前に断る
// (std::vector の確保失敗は例外無効ビルドでは abort = 再起動になる)。
tl::expected<void, Error> render_whole(std::u32string_view kana, const Options& opt,
                                       std::vector<std::int16_t>& out, std::vector<VisemeEvent>* visemes) {
    const auto est_samples = static_cast<std::size_t>(
        internal::estimate_utterance_ms(kana, opt.mora_ms) * static_cast<float>(opt.sample_rate_hz) / 1000.0f);
    if (!internal::pcm_fits_in_memory(est_samples)) {
        return tl::make_unexpected(Error::OutOfMemory);
    }

    std::vector<Mora> moras;
    if (!internal::parse_kana(kana, moras)) {
        return tl::make_unexpected(Error::InvalidKana);
    }
    internal::apply_devoicing(moras);

    // 単位連結エンジン: DB があり、必要な単位が全部揃っていれば
    // render_units が out を埋めて true。欠け/未ロード/サンプルレート不一致は
    // フォルマントへ。
    if (opt.engine != Engine::Formant) {
        auto db = g_voice_db.load();
        if (db && db->sample_rate() == opt.sample_rate_hz &&
            internal::render_units(moras, *db, out, opt)) {
            return {};
        }
    }

    std::vector<internal::Segment> segs;
    internal::build_segments(moras, segs, opt);
    if (segs.empty()) {
        return tl::make_unexpected(Error::InvalidKana);
    }
    apply_formant_scale(segs, opt.formant_scale);
    internal::apply_prosody(segs, opt);

    std::size_t estimated_samples = 0;
    for (const auto& s : segs) {
        estimated_samples += static_cast<std::size_t>(s.duration_ms * 0.001f * opt.sample_rate_hz) + 16;
    }
    out.reserve(estimated_samples);

    if (visemes) collect_visemes(segs, *visemes);

    internal::render_segments(segs, out, opt);
    return {};
}

// 一括合成。戻り値は出力 PCM のサンプルレート (sanoTTS は 22.05 kHz、他は opt.sample_rate_hz)。
tl::expected<std::uint32_t, Error> synthesize_impl(std::u32string_view kana, std::vector<std::int16_t>& out,
                                                   std::vector<VisemeEvent>* visemes, const Options& opt_in,
                                                   bool allow_native_rate) {
    out.clear();
    if (visemes) visemes->clear();
    Options opt = resolve_defaults(opt_in);

    // 発話が長すぎて PCM が空きメモリに収まらないなら、どのエンジンでも合成せず断る。
    const std::uint32_t est_rate =
        wants_sano(opt) && sano_weights_loaded() ? std::max<std::uint32_t>(opt.sample_rate_hz, 22050u) : opt.sample_rate_hz;
    const auto est_samples = static_cast<std::size_t>(
        internal::estimate_utterance_ms(kana, opt.mora_ms) * static_cast<float>(est_rate) / 1000.0f);
    if (!internal::pcm_fits_in_memory(est_samples)) {
        return tl::make_unexpected(Error::OutOfMemory);
    }

    // sanoTTS エンジン: 重みがロード済みなら最優先。口形は音素の継続長から作る。
    if (wants_sano(opt) && (allow_native_rate || opt.sample_rate_hz == 22050u)) {
        std::uint32_t rate = 0;
        if (internal::render_sano(kana, out, opt, rate, visemes)) {
            return rate;
        }
        out.clear();
        if (visemes) visemes->clear();
    }

    // HMM エンジン: ボイスがロード済みなら次に優先 (品質最良)。
    // アクセント記号 (' と /) は HMM のみ解釈し、他エンジンでは
    // parse_kana が読み飛ばす。全チャンクを 1 本の PCM に連結する。
    if (wants_hmm(opt) && hmm_voice_loaded()) {
        // PCM は最初に 1 回だけ確保し (途中の再確保 = 旧 + 新の一時倍増を避ける)、
        // 合成後に余りを返す。
        out.reserve(est_samples);
        std::vector<VisemeEvent> scratch;
        internal::VisemeBuilder builder(visemes ? *visemes : scratch);
        const auto outcome = internal::render_hmm_stream(
            kana, opt,
            [&](std::vector<std::int16_t>&& pcm, std::vector<internal::VisemeSpan>&& spans, const std::u32string&) {
                out.insert(out.end(), pcm.begin(), pcm.end());
                for (const auto& sp : spans) builder.add(sp.vowel, sp.duration_ms);
                return true;
            },
            /*stream=*/false);
        if (outcome == internal::StreamOutcome::Ok) {
            builder.finish();
            if (out.capacity() - out.size() > 32 * 1024) out.shrink_to_fit();
            return opt.sample_rate_hz;
        }
        // 諦めた (メモリ不足など): 途中まで作った分は捨てて他エンジンへ。
        out.clear();
        if (visemes) visemes->clear();
    }

    if (auto r = render_whole(kana, opt, out, visemes); !r) return tl::make_unexpected(r.error());
    return opt.sample_rate_hz;
}

}  // namespace

namespace internal {

void VisemeBuilder::add(Vowel v, float duration_ms) {
    if (duration_ms <= 0.0f) return;
    if (!have_last_ || v != last_) {
        out_.push_back({static_cast<std::uint32_t>(t_ms_ + 0.5f), v});
        last_ = v;
        have_last_ = true;
    }
    t_ms_ += duration_ms;
}

void spans_to_events(std::span<const VisemeSpan> spans, std::vector<VisemeEvent>& out) {
    out.clear();
    VisemeBuilder b(out);
    for (const auto& sp : spans) b.add(sp.vowel, sp.duration_ms);
    b.finish();
}

void VisemeBuilder::finish() {
    if (have_last_ && last_ != Vowel::None) {
        out_.push_back({static_cast<std::uint32_t>(t_ms_ + 0.5f), Vowel::None});
        last_ = Vowel::None;
    }
}

}  // namespace internal

tl::expected<void, Error> synthesize(std::u32string_view kana, std::vector<std::int16_t>& out,
                                     const Options& opt) {
    auto r = synthesize_impl(kana, out, nullptr, opt, /*allow_native_rate=*/false);
    if (!r) return tl::make_unexpected(r.error());
    return {};
}

tl::expected<void, Error> synthesize(std::u32string_view kana, std::vector<std::int16_t>& out,
                                     std::vector<VisemeEvent>& visemes, const Options& opt) {
    auto r = synthesize_impl(kana, out, &visemes, opt, /*allow_native_rate=*/false);
    if (!r) return tl::make_unexpected(r.error());
    return {};
}

tl::expected<std::uint32_t, Error> synthesize_ex(std::u32string_view kana,
                                                 std::vector<std::int16_t>& out, const Options& opt) {
    return synthesize_impl(kana, out, nullptr, opt, /*allow_native_rate=*/true);
}

tl::expected<void, Error> synthesize_stream(std::u32string_view kana, const ChunkSink& sink, const Options& opt_in) {
    const Options opt = resolve_defaults(opt_in);

    // sanoTTS: 句 (、。) ごとに合成し、句間に HMM と同じ長さの無音を入れて 1 句ずつ渡す
    // (22.05 kHz)。重み未ロード / 最初の句の失敗なら何も渡さず次のエンジンへ。
    if (wants_sano(opt)) {
        const auto outcome = internal::render_sano_stream(
            kana, opt,
            [&](std::vector<std::int16_t>&& pcm, std::uint32_t rate_hz, std::vector<internal::VisemeSpan>&& spans,
                const std::u32string& text) {
                SynthChunk chunk;
                chunk.text = text;
                chunk.pcm = std::move(pcm);
                chunk.sample_rate = rate_hz;
                internal::spans_to_events(spans, chunk.visemes);
                return sink(std::move(chunk));
            });
        switch (outcome) {
            case internal::StreamOutcome::Ok: return {};
            case internal::StreamOutcome::Cancelled: return tl::make_unexpected(Error::Cancelled);
            // 途中まで渡してしまった分は取り消せないので、他エンジンでやり直さない。
            case internal::StreamOutcome::Aborted: return tl::make_unexpected(Error::OutOfMemory);
            case internal::StreamOutcome::NoOutput: break;  // 何も渡していない → 他エンジンへ
        }
    }

    if (wants_hmm(opt) && hmm_voice_loaded()) {
        const auto outcome = internal::render_hmm_stream(
            kana, opt,
            [&](std::vector<std::int16_t>&& pcm, std::vector<internal::VisemeSpan>&& spans, const std::u32string& text) {
                SynthChunk chunk;
                chunk.text = text;
                chunk.pcm = std::move(pcm);
                chunk.sample_rate = opt.sample_rate_hz;
                internal::spans_to_events(spans, chunk.visemes);
                return sink(std::move(chunk));
            },
            /*stream=*/true);
        switch (outcome) {
            case internal::StreamOutcome::Ok: return {};
            case internal::StreamOutcome::Cancelled: return tl::make_unexpected(Error::Cancelled);
            // 途中まで渡してしまった分は取り消せないので、他エンジンでやり直さない。
            case internal::StreamOutcome::Aborted: return tl::make_unexpected(Error::OutOfMemory);
            case internal::StreamOutcome::NoOutput: break;  // 何も渡していない → 他エンジンへ
        }
    }

    // HMM 以外 / HMM を使えなかった: 発話全体が 1 チャンク。
    SynthChunk chunk;
    chunk.text = std::u32string(kana);
    chunk.sample_rate = opt.sample_rate_hz;
    if (auto r = render_whole(kana, opt, chunk.pcm, &chunk.visemes); !r) return r;
    if (chunk.pcm.empty()) return tl::make_unexpected(Error::InvalidKana);
    if (!sink(std::move(chunk))) return tl::make_unexpected(Error::Cancelled);
    return {};
}

}  // namespace stackchan::jtts
