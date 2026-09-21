// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <esp_timer.h>
#include <jtts/jtts.hpp>

#include "chunk_player.hpp"

namespace stackchan::app {

// Parse the user's jtts config JSON (the same blob Speech::configure
// expects — defaults to the built-in "female child" preset when the
// JSON is empty / malformed / missing fields) into a jtts::Options.
// Exposed so non-babble call sites (e.g. the settings page /api/jtts-say
// + BLE chr 0x2d test-speak buttons in app_main) use exactly the same
// voice / pitch / mora / formant settings as the demo_loop babble.
jtts::Options resolve_speech_options(const std::string& json,
                                     std::uint32_t sample_rate);

// Synthesises a short "babble" speech-like utterance and plays it through
// M5.Speaker. While the clip is playing, `current_mouth()` returns the mouth
// pose the render task should show: the vowel (あ/い/う/え/お) being spoken
// when the engine can report one (jtts formant / HMM engines), otherwise the
// instantaneous envelope (peak amplitude) of the audio.
class Speech {
public:
    // Sample rate of synthesised audio. 16 kHz int16.
    static constexpr std::uint32_t kSampleRate = 16'000;

    // Envelope window — one envelope sample covers this many ms of audio.
    static constexpr std::uint32_t kEnvelopeStepMs = 16;

    // Override the compile-time voice preset and phrase list with values from
    // a user-supplied JSON document (delivered over BLE, persisted in NVS).
    // Missing or invalid fields fall back to the defaults — invalid JSON is
    // ignored entirely. Call once at startup, before the first babble.
    void configure(const std::string& json);

    // Which phrase babble() picks next. Random (default) uses the caller's
    // seed; Sequential walks the phrase list top to bottom and wraps around.
    enum class PhraseOrder : std::uint8_t { Random, Sequential };

    // Start a fresh utterance (non-blocking — synthesis runs in its own task and
    // M5.Speaker queues the audio). Random order: `seed` selects the phrase
    // (seed % phrase count). Sequential order: `seed` is ignored, the next line
    // in the list is used (restarting from the first line after configure()).
    // Returns the *display* text (発話内容) of the chosen phrase — synthesis uses
    // that phrase's separate *reading* (発声内容, kana). Returns an empty string
    // only when there are no phrases.
    //
    // The balloon is driven through set_subtitle_sink(): each chunk's part of the
    // display text is shown as that chunk starts sounding (if nothing could be
    // synthesised the whole text is shown once, so the caller need not show it).
    std::string babble(std::uint32_t seed);

    // Speak an arbitrary kana string (発声内容; jtts has no kanji dictionary).
    // Same synthesis + lip-sync path as babble() so the avatar's mouth moves.
    // Non-blocking: synthesis runs in its own task (sanoTTS takes 1-2 s per
    // utterance; blocking the caller — demo_loop — would stall M5.update() and
    // drop touches). Returns false when the reading is empty, the previous
    // synthesis is still running, or the task could not be created. Caller-side
    // balloon text is the caller's business (it usually differs from the reading).
    // A new utterance cuts off one that is still playing once its first chunk is ready.
    //
    // Long text is synthesised chunk by chunk and *played while the next chunk
    // is being synthesised*: sound starts after the first chunk (≈ 1 s) instead
    // of after the whole utterance.
    bool say(std::u32string_view reading);

    // Cancel any in-flight babble so we can hand the speaker / I2S bus to
    // someone else (e.g. mic loopback). Discards a synthesis in progress (its
    // task ends after the chunk it is on) and stops the audio. Safe to call
    // from any task. is_speaking() stays true until that task has exited.
    void stop();

    // Mouth pose at "now". `open` is 0..1 (0 = closed); `form` is 0 = wide ..
    // 1 = narrow, or -1 when the pose comes from the audio envelope and the
    // face should follow `open` alone. Both are 0 / -1 when nothing is playing.
    struct Mouth {
        float open = 0.0f;
        float form = -1.0f;
    };
    Mouth current_mouth() const;

    // Called every 20 ms with the current mouth while an utterance is being
    // synthesised / played, from the esp_timer task: keep it tiny (atomic
    // stores). Independent of how often the caller polls current_mouth(). Set
    // before the first say().
    using MouthSink = std::function<void(const Mouth&)>;
    void set_mouth_sink(MouthSink sink);

    // Balloon text synchronised with the chunks: babble() shows each chunk's part
    // of the display text (発話内容) at the moment that chunk starts sounding, using
    // the punctuation-based mapping in jtts/subtitle.hpp (falls back to showing the
    // whole text with the first chunk when the punctuation of reading and display
    // do not line up). `hold_ms` is the chunk's audio length. Called on the
    // esp_timer task and on the synthesis task (first chunk / failure): keep it
    // cheap. Set before the first babble().
    using SubtitleSink = std::function<void(const std::string& text, std::uint32_t hold_ms)>;
    void set_subtitle_sink(SubtitleSink sink);

    bool is_speaking() const;

private:
    // Speaker channel for jtts speech (conversation playback uses 0).
    static constexpr int kSpeakerChannel = 1;
    // Mouth update period while speaking.
    static constexpr std::uint32_t kMouthTimerUs = 20'000;

    // Pre-computed envelope (peak amplitude per kEnvelopeStepMs window),
    // normalised to 0..1, indexed by elapsed window count. Only filled for
    // chunks the engine gave no vowel timeline for (unit-concatenation).
    std::vector<float> envelope_;
    // Vowel timeline of the current utterance in ms since start_ms_ (jtts
    // formant / HMM engines). Chunk timelines are appended at the chunk's
    // scheduled playback start, so a synthesis underrun (gap) keeps the mouth
    // closed and later chunks stay in sync with the sound.
    std::vector<jtts::VisemeEvent> visemes_;
    // Chunk PCM queue for M5.Speaker (keeps buffers alive while they play).
    ChunkPlayer player_{kSpeakerChannel};

    // Subtitle timeline of the current utterance: `at_ms` is relative to start_ms_.
    struct Subtitle {
        std::uint32_t at_ms = 0;
        std::uint32_t hold_ms = 0;
        std::string text;
    };
    std::vector<Subtitle> subs_;      // guarded by buf_mutex_
    std::size_t next_sub_ = 0;        // guarded by buf_mutex_: first entry not yet shown

    // A single babble phrase. `display` (発話内容) is the UTF-8 text shown in
    // the balloon — free-form, may contain kanji/punctuation. `reading`
    // (発声内容) is the kana fed to jtts for synthesis (jtts has no kana-to-
    // phoneme dictionary, so kanji in a reading are silently skipped). The
    // two are decoupled on purpose so "こんにちは" can be read "こんにちわ".
    struct Phrase {
        std::string display;
        std::u32string reading;
    };

    // Voice preset + babble phrase list. configure() can overwrite these at
    // boot; otherwise they hold the compile-time defaults (Female child
    // preset, ~8 short Japanese phrases).
    jtts::Options opts_;
    std::vector<Phrase> phrases_;
    PhraseOrder phrase_order_{PhraseOrder::Random};
    std::size_t next_phrase_{0}; // Sequential: index of the phrase babble() speaks next
    bool initialised_{false};

    // Playback start of the first chunk / total scheduled length so far (grows
    // as chunks are queued). 0 start = idle.
    std::atomic<std::uint32_t> start_ms_{0};
    std::atomic<std::uint32_t> duration_ms_{0};

    // 合成は別タスクで行う (sanoTTS は 1 発話 1〜2 秒かかり、呼び出し元 = demo_loop を
    // ブロックすると M5.update() が止まってタッチを取りこぼす)。synthesizing_ の間は
    // is_speaking() が true。stop() は gen_ を進めて進行中の合成結果を捨てる。
    std::atomic<bool> synthesizing_{false};
    std::atomic<std::uint32_t> gen_{0};
    // envelope_ / visemes_ / subs_ / start_ms_ / duration_ms_ の更新 (合成タスク) と
    // 読み出し (口のタイマー / demo_loop) を直列化。
    mutable std::mutex buf_mutex_;
    struct SynthJob;
    static void synth_task(void* arg);
    // 合成タスクの本体: チャンクごとに合成 → 再生キューへ → 口形 / 吹き出しの時刻を記録。
    void run_utterance(const SynthJob& job);
    bool say_impl(std::u32string_view reading, const std::string* display);

    MouthSink mouth_sink_;
    SubtitleSink subtitle_sink_;
    esp_timer_handle_t mouth_timer_ = nullptr;
    void pump_subtitles();
    void start_mouth_timer();
    void stop_mouth_timer();
    void on_mouth_timer();
};

} // namespace stackchan::app
