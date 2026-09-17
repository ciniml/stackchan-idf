// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/task_stack.hpp>
#include "speech.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

    // phrases: array whose elements are either
    //   - a string  "こんにちわ"                       (display == reading), or
    //   - an object  {"text":"こんにちは","reading":"こんにちわ"}
    // `reading` defaults to `text` when omitted, and vice-versa, so a phrase
    // can supply either field alone.
    const cJSON* phrases = cJSON_GetObjectItemCaseSensitive(root, "phrases");
    if (cJSON_IsArray(phrases)) {
        std::vector<Phrase> parsed;
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, phrases) {
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
        if (!parsed.empty()) phrases_ = std::move(parsed);
    }
    cJSON_Delete(root);
    ESP_LOGI(kTag, "jtts config: voice=%s f0=%.0f mora=%.0fms phrases=%zu",
             opts_.voice == jtts::Voice::Female ? "female" : "male",
             opts_.f0_hz, opts_.mora_ms, phrases_.size());
}

std::string Speech::babble(std::uint32_t seed)
{
    if (!initialised_) {
        configure(""); // first-call lazy init with defaults
    }
    if (phrases_.empty()) {
        return {};
    }
    const Phrase& phrase = phrases_[seed % phrases_.size()];
    // Couldn't pronounce → still return the display text so the caller shows
    // the matching balloon (no audio / mouth movement in that case).
    (void)say(phrase.reading);
    return phrase.display;
}

struct Speech::SynthJob {
    Speech* self;
    std::u32string reading;
    jtts::Options opt;
    std::uint32_t gen;
};

void Speech::synth_task(void* arg)
{
    // vTaskDeleteWithCaps() never returns, so every local (the old PCM buffer
    // handed back by swap(), the envelope, the job) must be destroyed BEFORE
    // it is called — otherwise ~60 KB leak per utterance. Hence the body
    // lives in an immediately-invoked lambda and the delete happens after it.
    [&] {
    std::unique_ptr<SynthJob> job{static_cast<SynthJob*>(arg)};
    Speech* self = job->self;
    std::vector<std::int16_t> pcm;
    auto r = jtts::synthesize_ex(job->reading, pcm, job->opt);
    if (!r || pcm.empty() || self->gen_.load(std::memory_order_acquire) != job->gen) {
        // 合成失敗、無音、または stop() で取り消された。
        self->synthesizing_.store(false, std::memory_order_release);
        return;
    }
    const std::uint32_t rate = *r;
    std::vector<float> envelope;
    build_envelope_from_pcm(pcm, envelope, rate, kEnvelopeStepMs);
    {
        std::lock_guard<std::mutex> lock(self->buf_mutex_);
        self->pcm_.swap(pcm);
        self->envelope_.swap(envelope);
        self->play_rate_ = rate;
        self->duration_ms_.store(
            static_cast<std::uint32_t>(static_cast<float>(self->pcm_.size()) * 1000.0f /
                                       static_cast<float>(rate)),
            std::memory_order_relaxed);
        self->start_ms_.store(static_cast<std::uint32_t>(esp_timer_get_time() / 1000),
                              std::memory_order_release);
        M5.Speaker.playRaw(self->pcm_.data(), self->pcm_.size(), rate, /*stereo=*/false,
                           /*repeat=*/1, /*channel=*/-1, /*stop_current_sound=*/true);
    }
    self->synthesizing_.store(false, std::memory_order_release);
    }();
    vTaskDeleteWithCaps(nullptr);
}

bool Speech::say(std::u32string_view reading)
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
    auto* job = new SynthJob{this, std::u32string{reading}, opt, gen_.load(std::memory_order_acquire)};
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
    // 進行中の合成があれば結果を捨てさせる (タスク自体は合成完了まで走る)。
    gen_.fetch_add(1, std::memory_order_acq_rel);
    if (M5.Speaker.isPlaying()) {
        M5.Speaker.stop();
    }
    start_ms_.store(0, std::memory_order_release);
    duration_ms_.store(0, std::memory_order_release);
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

float Speech::current_mouth_open() const
{
    std::lock_guard<std::mutex> lock(buf_mutex_);
    const std::uint32_t start = start_ms_.load(std::memory_order_acquire);
    if (start == 0 || envelope_.empty()) {
        return 0.0f;
    }
    const std::uint32_t now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    const std::uint32_t elapsed = now - start;
    if (elapsed >= duration_ms_.load(std::memory_order_relaxed)) {
        return 0.0f;
    }
    const std::size_t idx = elapsed / kEnvelopeStepMs;
    if (idx >= envelope_.size()) {
        return 0.0f;
    }
    return envelope_[idx];
}

} // namespace stackchan::app
