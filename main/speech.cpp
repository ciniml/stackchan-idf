// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/task_stack.hpp>
#include "speech.hpp"
#include "utf8.hpp"
#include <jtts/subtitle.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

#include <M5Unified.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "speech";

// Compile-time defaults — used when no JttsConfig has been written over BLE
// (fresh device, or empty JSON). Each entry is { display, reading }: the
// balloon shows `display` (free-form, kanji allowed) while jtts synthesises
// `reading` (kana only — kanji in a reading are silently skipped). Keeping
// them separate lets us spell "こんにちは" on screen but pronounce the
// natural "こんにちわ".
struct DefaultPhrase {
    std::string_view display;        // UTF-8
    std::u32string_view reading;     // kana for jtts
};
constexpr DefaultPhrase kDefaultPhrases[] = {
    {"こんにちは",        U"こんにちわ"},
    {"おはよう",          U"おはよー"},
    {"やっほー",          U"やっほー"},
    {"あそぼうよ",        U"あそぼーよ"},
    {"なでなでして",      U"なでなで してー"},
    {"おなかすいた",      U"おなか すいたー"},
    {"元気元気！",        U"げんき げんき"},
    {"スタックチャンです", U"すたっくちゃんです"},
};

jtts::Options default_options(std::uint32_t sample_rate)
{
    jtts::Options opt;
    opt.voice = jtts::Voice::Female;
    opt.f0_hz = 280.0f;       // child preset
    opt.formant_scale = 1.30f;
    opt.mora_ms = 120.0f;
    opt.sample_rate_hz = sample_rate;
    return opt;
}

// cJSON helpers. Numeric fields are accepted as JSON numbers; voice is a
// string ("male"/"female"); missing fields keep their current value.
void apply_voice(jtts::Options& opt, const cJSON* item)
{
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return;
    if (std::strcmp(item->valuestring, "male") == 0) opt.voice = jtts::Voice::Male;
    else if (std::strcmp(item->valuestring, "female") == 0) opt.voice = jtts::Voice::Female;
}

void apply_number(float& dst, const cJSON* item)
{
    if (cJSON_IsNumber(item)) dst = static_cast<float>(item->valuedouble);
}

// synth は文字列 ("v2"/"classic")。missing / 不明値は現在値を維持。
void apply_synth(jtts::Options& opt, const cJSON* item)
{
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return;
    if (std::strcmp(item->valuestring, "classic") == 0) {
        opt.synth = jtts::SynthVariant::Classic;
    } else if (std::strcmp(item->valuestring, "v2") == 0) {
        opt.synth = jtts::SynthVariant::V2;
    }
}

// engine は文字列 ("auto"/"formant"/"unit"/"hmm")。missing / 不明値は現在値を維持。
void apply_engine(jtts::Options& opt, const cJSON* item)
{
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return;
    if (std::strcmp(item->valuestring, "auto") == 0) {
        opt.engine = jtts::Engine::Auto;
    } else if (std::strcmp(item->valuestring, "formant") == 0) {
        opt.engine = jtts::Engine::Formant;
    } else if (std::strcmp(item->valuestring, "unit") == 0) {
        opt.engine = jtts::Engine::Unit;
    } else if (std::strcmp(item->valuestring, "hmm") == 0) {
        opt.engine = jtts::Engine::Hmm;
    } else if (std::strcmp(item->valuestring, "sano") == 0) {
        opt.engine = jtts::Engine::Sano;
    }
}

// Mouth pose per vowel, in the avatar's terms: `open` scales the rectangle's
// height, `form` its width (0 = wide, 1 = narrow). None = lips closed.
//   あ: tall, a little narrower than at rest    い: wide and flat
//   う: narrow, small opening                   え: wide, half open
//   お: narrow-ish, fairly tall (rounded)
constexpr Speech::Mouth kClosedMouth{0.0f, 0.0f};

constexpr Speech::Mouth mouth_for_vowel(jtts::Vowel v) noexcept
{
    switch (v) {
    case jtts::Vowel::A: return {1.00f, 0.30f};
    case jtts::Vowel::I: return {0.25f, 0.00f};
    case jtts::Vowel::U: return {0.30f, 1.00f};
    case jtts::Vowel::E: return {0.55f, 0.15f};
    case jtts::Vowel::O: return {0.75f, 0.85f};
    case jtts::Vowel::None: break;
    }
    return kClosedMouth;
}

// Time to glide from the previous vowel's pose to the next one. Short enough
// to keep fast speech crisp, long enough that the lips don't snap.
constexpr float kMouthBlendMs = 60.0f;

void build_envelope_from_pcm(const std::vector<std::int16_t>& pcm,
                             std::vector<float>& envelope, std::uint32_t sample_rate,
                             std::uint32_t step_ms)
{
    const std::size_t window =
        static_cast<std::size_t>(sample_rate) * static_cast<std::size_t>(step_ms) / 1000u;
    if (window == 0 || pcm.empty()) {
        envelope.clear();
        return;
    }
    const std::size_t windows = (pcm.size() + window - 1) / window;
    envelope.assign(windows, 0.0f);
    for (std::size_t w = 0; w < windows; ++w) {
        const std::size_t begin = w * window;
        const std::size_t end = std::min(begin + window, pcm.size());
        std::int32_t peak = 0;
        for (std::size_t i = begin; i < end; ++i) {
            peak = std::max(peak, std::abs(static_cast<std::int32_t>(pcm[i])));
        }
        envelope[w] = static_cast<float>(peak) / 32767.0f;
    }
}

// Pull voice / pitch / mora / formant / gain / vibrato out of a JSON
// blob into a jtts::Options. Missing fields stay at the input defaults
// (so caller seeds with default_options() / the current preset). Helper
// shared between Speech::configure and the file-static loader below.
void apply_options_json(jtts::Options& opt, const cJSON* root)
{
    if (root == nullptr) return;
    apply_voice(opt, cJSON_GetObjectItemCaseSensitive(root, "voice"));
    apply_number(opt.f0_hz, cJSON_GetObjectItemCaseSensitive(root, "f0_hz"));
    apply_number(opt.formant_scale, cJSON_GetObjectItemCaseSensitive(root, "formant_scale"));
    apply_number(opt.mora_ms, cJSON_GetObjectItemCaseSensitive(root, "mora_ms"));
    apply_number(opt.gain, cJSON_GetObjectItemCaseSensitive(root, "gain"));
    apply_number(opt.breathiness, cJSON_GetObjectItemCaseSensitive(root, "breathiness"));
    apply_number(opt.voicing_mul, cJSON_GetObjectItemCaseSensitive(root, "voicing_mul"));
    apply_number(opt.frication_mul, cJSON_GetObjectItemCaseSensitive(root, "frication_mul"));
    apply_number(opt.vibrato_rate_hz, cJSON_GetObjectItemCaseSensitive(root, "vibrato_rate_hz"));
    apply_number(opt.vibrato_cents, cJSON_GetObjectItemCaseSensitive(root, "vibrato_cents"));
    // やわらかさ系 (V2 のみ有効、bw_scale は Classic でも効く) + 合成方式。
    apply_number(opt.glottal_oq, cJSON_GetObjectItemCaseSensitive(root, "glottal_oq"));
    apply_number(opt.tilt_db, cJSON_GetObjectItemCaseSensitive(root, "tilt_db"));
    apply_number(opt.bw_scale, cJSON_GetObjectItemCaseSensitive(root, "bw_scale"));
    apply_synth(opt, cJSON_GetObjectItemCaseSensitive(root, "synth"));
    apply_engine(opt, cJSON_GetObjectItemCaseSensitive(root, "engine"));
    // HMM エンジンのみ: ボイス既定ピッチからの半音シフト
    apply_number(opt.hmm_half_tone, cJSON_GetObjectItemCaseSensitive(root, "hmm_half_tone"));
}

} // namespace

jtts::Options resolve_speech_options(const std::string& json, std::uint32_t sample_rate)
{
    jtts::Options opt = default_options(sample_rate);
    if (json.empty()) return opt;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "resolve_speech_options: JSON parse failed, using defaults");
        return opt;
    }
    apply_options_json(opt, root);
    cJSON_Delete(root);
    return opt;
}

void Speech::configure(const std::string& json)
{
    // Compile-time fallback always runs first so configure() is idempotent
    // and missing JSON fields don't pick up stale state.
    opts_ = default_options(kSampleRate);
    phrases_.clear();
    for (const auto& p : kDefaultPhrases) {
        phrases_.push_back({std::string(p.display), std::u32string(p.reading)});
    }
    phrase_order_ = PhraseOrder::Random;
    next_phrase_ = 0;
    initialised_ = true;

    if (json.empty()) {
        return;
    }
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "jtts config: JSON parse failed, using defaults");
        return;
    }

    apply_options_json(opts_, root);

    // phrase_order: "random" (default) or "sequential" (top to bottom, looping).
    // Unknown / missing values keep the default.
    const cJSON* order = cJSON_GetObjectItemCaseSensitive(root, "phrase_order");
    if (cJSON_IsString(order) && order->valuestring != nullptr &&
        std::strcmp(order->valuestring, "sequential") == 0) {
        phrase_order_ = PhraseOrder::Sequential;
    }

    // phrases: array whose elements are either
    //   - a string  "こんにちわ"                       (display == reading), or
    //   - an object  {"text":"こんにちは","reading":"こんにちわ"}
    // `reading` defaults to `text` when omitted, and vice-versa, so a phrase
    // can supply either field alone.
    // Same element format for the proximity phrase list (spoken when a hand
    // comes close to the CoreS3 proximity sensor). Empty = balloon only.
    const auto parse_phrases = [](const cJSON* arr, std::vector<Phrase>& out) {
        if (!cJSON_IsArray(arr)) return false;
        std::vector<Phrase> parsed;
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, arr) {
            const char* display = nullptr;
            const char* reading = nullptr;
            if (cJSON_IsString(item) && item->valuestring != nullptr) {
                display = reading = item->valuestring;
            } else if (cJSON_IsObject(item)) {
                const cJSON* t = cJSON_GetObjectItemCaseSensitive(item, "text");
                const cJSON* r = cJSON_GetObjectItemCaseSensitive(item, "reading");
                if (cJSON_IsString(t) && t->valuestring != nullptr) display = t->valuestring;
                if (cJSON_IsString(r) && r->valuestring != nullptr) reading = r->valuestring;
                if (display == nullptr) display = reading;
                if (reading == nullptr) reading = display;
            }
            if (display == nullptr || reading == nullptr) continue;
            auto kana = decode_utf8(reading);
            if (kana.empty()) continue;          // nothing speakable → drop
            parsed.push_back({std::string(display), std::move(kana)});
        }
        if (parsed.empty()) return false;
        out = std::move(parsed);
        return true;
    };
    parse_phrases(cJSON_GetObjectItemCaseSensitive(root, "phrases"), phrases_);
    proximity_phrases_.clear();
    parse_phrases(cJSON_GetObjectItemCaseSensitive(root, "proximity_phrases"), proximity_phrases_);
    cJSON_Delete(root);
    ESP_LOGI(kTag, "jtts config: voice=%s f0=%.0f mora=%.0fms phrases=%zu order=%s",
             opts_.voice == jtts::Voice::Female ? "female" : "male",
             opts_.f0_hz, opts_.mora_ms, phrases_.size(),
             phrase_order_ == PhraseOrder::Sequential ? "sequential" : "random");
}

std::string Speech::speak_proximity(std::uint32_t seed)
{
    if (!initialised_) {
        configure("");
    }
    if (proximity_phrases_.empty()) {
        return {};
    }
    const Phrase& phrase = proximity_phrases_[seed % proximity_phrases_.size()];
    if (!say_impl(phrase.reading, &phrase.display) && subtitle_sink_) {
        subtitle_sink_(phrase.display, 0);
    }
    return phrase.display;
}

std::string Speech::babble(std::uint32_t seed)
{
    if (!initialised_) {
        configure(""); // first-call lazy init with defaults
    }
    if (phrases_.empty()) {
        return {};
    }
    std::size_t index;
    if (phrase_order_ == PhraseOrder::Sequential) {
        index = next_phrase_ % phrases_.size();
        next_phrase_ = index + 1; // stays < size + 1, so it never overflows
    } else {
        index = seed % phrases_.size();
    }
    const Phrase& phrase = phrases_[index];
    // Couldn't start (busy / task create failed) → still show the text once, so
    // the caller's balloon is never lost. Otherwise the subtitle sink is driven
    // by the synthesis task, chunk by chunk (and shows the whole text if nothing
    // could be synthesised). The display text is returned either way.
    if (!say_impl(phrase.reading, &phrase.display) && subtitle_sink_) {
        subtitle_sink_(phrase.display, 0);
    }
    return phrase.display;
}

struct Speech::SynthJob {
    Speech* self;
    std::u32string reading;
    std::string display;  // 吹き出しに出す表示テキスト (空 = 吹き出しは呼び出し側に任せる)
    jtts::Options opt;
    std::uint32_t gen;
};

void Speech::synth_task(void* arg)
{
    // vTaskDeleteWithCaps() never returns, so every local (the job, PCM buffers,
    // the subtitle mapper, ...) must be destroyed BEFORE it is called —
    // otherwise ~60 KB leak per utterance. Hence the body lives in an
    // immediately-invoked lambda and the delete happens after it.
    [&] {
        std::unique_ptr<SynthJob> job{static_cast<SynthJob*>(arg)};
        Speech* self = job->self;
        self->run_utterance(*job);
        self->synthesizing_.store(false, std::memory_order_release);
    }();
    vTaskDeleteWithCaps(nullptr);
}

void Speech::run_utterance(const SynthJob& job)
{
    // Balloon text follows the chunks (only when there is text and a sink).
    std::optional<jtts::SubtitleMapper> subtitles;
    if (!job.display.empty() && subtitle_sink_) {
        subtitles.emplace(job.display, job.reading);
    }

    // stop() bumps gen_: everything after that is discarded.
    const auto cancelled = [&] { return gen_.load(std::memory_order_acquire) != job.gen; };

    bool first = true;
    bool any = false;
    const auto on_chunk = [&](jtts::SynthChunk&& chunk) -> bool {
        const std::size_t samples = chunk.pcm.size();
        if (samples == 0) {
            return true;
        }
        if (cancelled()) {
            return false;
        }
        // sanoTTS outputs 22.05 kHz; the chunk carries the actual rate.
        const std::uint32_t rate = chunk.sample_rate != 0 ? chunk.sample_rate : job.opt.sample_rate_hz;

        // This chunk's part of the balloon text (chunks arrive in order).
        std::string subtitle;
        if (subtitles) {
            subtitle = subtitles->next(chunk.text);
        }
        // Envelope (only for engines without a vowel timeline) must be taken
        // before the PCM is handed to the player.
        std::vector<float> env;
        if (chunk.visemes.empty()) {
            build_envelope_from_pcm(chunk.pcm, env, rate, kEnvelopeStepMs);
        }
        const auto dur_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(samples) * 1000u / rate);

        if (first) {
            // A previous utterance may still be sounding: cut it now that the new
            // one has audio, and free its buffers.
            player_.begin();
        }

        // Queue the chunk (blocks while the speaker's 2 slots are full) and learn
        // when it will actually start sounding.
        const auto start_opt = player_.enqueue(std::move(chunk.pcm), rate, cancelled);
        if (!start_opt) {
            return false; // stop() came in
        }
        const std::uint32_t start = *start_opt;

        {
            std::lock_guard<std::mutex> lock(buf_mutex_);
            if (first) {
                visemes_.clear();
                envelope_.clear();
                subs_.clear();
                next_sub_ = 0;
                duration_ms_.store(dur_ms, std::memory_order_relaxed);
                // 0 means "idle", so never publish a start of exactly 0.
                start_ms_.store(start != 0 ? start : 1, std::memory_order_release);
            }
            const std::uint32_t base = start_ms_.load(std::memory_order_relaxed);
            const std::uint32_t offset = first ? 0 : start - base;
            for (const auto& e : chunk.visemes) {
                visemes_.push_back({e.start_ms + offset, e.vowel});
            }
            if (!env.empty()) {
                if (envelope_.size() < offset / kEnvelopeStepMs) {
                    envelope_.resize(offset / kEnvelopeStepMs, 0.0f); // silence up to the chunk
                }
                envelope_.insert(envelope_.end(), env.begin(), env.end());
            }
            if (!subtitle.empty()) {
                subs_.push_back({offset, dur_ms, std::move(subtitle)});
            }
            duration_ms_.store(offset + dur_ms, std::memory_order_relaxed);
        }
        if (first) {
            // The first balloon text goes up right now, before the timer takes over.
            pump_subtitles();
            start_mouth_timer();
        }
        first = false;
        any = true;
        return true;
    };

    const auto r = jtts::synthesize_stream(job.reading, on_chunk, job.opt);
    if (!r && r.error() != jtts::Error::Cancelled) {
        ESP_LOGW(kTag, "say: synthesis %s%s", jtts::to_string(r.error()),
                 any ? " (utterance cut short)" : "");
    }
    if (cancelled()) {
        // stop() already stopped the speaker; give it a moment to finish reading the
        // last block, then free what we queued.
        vTaskDelay(pdMS_TO_TICKS(30));
        player_.release();
    } else if (!any && subtitle_sink_ && !job.display.empty()) {
        // Nothing could be synthesised: still show the text once.
        subtitle_sink_(job.display, 0);
    }
}

bool Speech::say(std::u32string_view reading)
{
    return say_impl(reading, nullptr);
}

bool Speech::say_impl(std::u32string_view reading, const std::string* display)
{
    if (!initialised_) {
        configure(""); // first-call lazy init with defaults
    }
    if (reading.empty()) return false;
    if (synthesizing_.exchange(true, std::memory_order_acq_rel)) {
        return false; // 前の合成がまだ走っている
    }
    jtts::Options opt = opts_;
    opt.sample_rate_hz = kSampleRate; // 他エンジンの既定レート。sanoTTS は 22.05 kHz を返す
    auto* job = new SynthJob{this, std::u32string{reading}, display != nullptr ? *display : std::string{},
                             opt, gen_.load(std::memory_order_acquire)};
    // スタックは PSRAM (flash への書き込みはしない)。CPU 0 — CPU 1 は描画 / サーボ / スピーカー。
    const BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(&synth_task, "speech_synth", 16 * 1024, job,
                                                          tskIDLE_PRIORITY + 2, nullptr, 0,
                                                          stackchan::kNoFlashTaskStackCaps);
    if (rc != pdPASS) {
        ESP_LOGE("speech", "synth task create failed");
        delete job;
        synthesizing_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void Speech::stop()
{
    stop_mouth_timer();
    // 進行中の合成があれば結果を捨てさせ (gen_)、鳴っている音を止める (タスク自体は今の
    // チャンクの合成が終わるまで走る)。gen_ の更新と停止は ChunkPlayer のミューテックスの
    // 下で行うので、stop() の後に遅れて鳴り出すチャンクは無い。
    player_.stop([this] { gen_.fetch_add(1, std::memory_order_acq_rel); });
    std::lock_guard<std::mutex> lock(buf_mutex_);
    start_ms_.store(0, std::memory_order_release);
    duration_ms_.store(0, std::memory_order_release);
    visemes_.clear();
    envelope_.clear();
    subs_.clear();
    next_sub_ = 0;
}

bool Speech::is_speaking() const
{
    if (synthesizing_.load(std::memory_order_acquire)) {
        return true;
    }
    const std::uint32_t start = start_ms_.load(std::memory_order_acquire);
    if (start == 0) {
        return false;
    }
    const std::uint32_t now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    return (now - start) < duration_ms_.load(std::memory_order_relaxed);
}

void Speech::set_mouth_sink(MouthSink sink)
{
    mouth_sink_ = std::move(sink);
}

void Speech::set_subtitle_sink(SubtitleSink sink)
{
    subtitle_sink_ = std::move(sink);
}

// Show the newest balloon text whose chunk has started sounding. If several are
// due (the timer was late) only the latest is shown — the earlier ones are stale.
void Speech::pump_subtitles()
{
    if (!subtitle_sink_) {
        return;
    }
    Subtitle due;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(buf_mutex_);
        const std::uint32_t start = start_ms_.load(std::memory_order_relaxed);
        if (start == 0) {
            return;
        }
        const std::uint32_t elapsed = static_cast<std::uint32_t>(esp_timer_get_time() / 1000) - start;
        while (next_sub_ < subs_.size() && elapsed >= subs_[next_sub_].at_ms) {
            due = subs_[next_sub_];
            have = true;
            ++next_sub_;
        }
    }
    if (have) {
        subtitle_sink_(due.text, due.hold_ms);
    }
}

void Speech::start_mouth_timer()
{
    if (!mouth_sink_ && !subtitle_sink_) {
        return;
    }
    if (mouth_timer_ == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = [](void* self) { static_cast<Speech*>(self)->on_mouth_timer(); };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "speech_mouth";
        args.skip_unhandled_events = true;
        if (esp_timer_create(&args, &mouth_timer_) != ESP_OK) {
            mouth_timer_ = nullptr;
            ESP_LOGW(kTag, "mouth timer create failed — mouth/balloon will not follow the chunks");
            return;
        }
    }
    // Already running (ESP_ERR_INVALID_STATE) is fine.
    (void)esp_timer_start_periodic(mouth_timer_, kMouthTimerUs);
}

void Speech::stop_mouth_timer()
{
    if (mouth_timer_ != nullptr) {
        (void)esp_timer_stop(mouth_timer_); // not running (ESP_ERR_INVALID_STATE) is fine
    }
}

void Speech::on_mouth_timer()
{
    if (mouth_sink_) {
        mouth_sink_(current_mouth());
    }
    pump_subtitles();
    // Nothing left to animate: close the mouth once and go quiet. is_speaking() is
    // also true while chunks are still being synthesised, so a slow chunk leaves
    // a gap, not an end.
    if (!is_speaking()) {
        (void)esp_timer_stop(mouth_timer_);
        if (mouth_sink_) {
            mouth_sink_(Mouth{});
        }
    }
}

Speech::Mouth Speech::current_mouth() const
{
    const std::uint32_t start = start_ms_.load(std::memory_order_acquire);
    if (start == 0) {
        return {};
    }
    const std::uint32_t now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    const std::uint32_t elapsed = now - start;

    std::lock_guard<std::mutex> lock(buf_mutex_);
    if (elapsed >= duration_ms_.load(std::memory_order_relaxed)) {
        return {};
    }

    if (!visemes_.empty()) {
        // Vowel lip-sync: find the event in effect and glide toward its pose
        // from the previous event's pose.
        const auto next = std::upper_bound(
            visemes_.begin(), visemes_.end(), elapsed,
            [](std::uint32_t t, const jtts::VisemeEvent& e) { return t < e.start_ms; });
        if (next == visemes_.begin()) {
            return {kClosedMouth.open, kClosedMouth.form};
        }
        const auto cur = std::prev(next);
        const Mouth to = mouth_for_vowel(cur->vowel);
        const Mouth from = cur == visemes_.begin() ? kClosedMouth : mouth_for_vowel(std::prev(cur)->vowel);
        float t = static_cast<float>(elapsed - cur->start_ms) / kMouthBlendMs;
        t = t > 1.0f ? 1.0f : t;
        t = t * t * (3.0f - 2.0f * t);  // smoothstep
        return {from.open + (to.open - from.open) * t, from.form + (to.form - from.form) * t};
    }

    // No vowel timeline (unit-concatenation): mouth follows the loudness.
    const std::size_t idx = elapsed / kEnvelopeStepMs;
    if (idx >= envelope_.size()) {
        return {};
    }
    return {envelope_[idx], -1.0f};
}

} // namespace stackchan::app
