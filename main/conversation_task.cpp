// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "conversation_task.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <M5Unified.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "avatar/expression.hpp"
#include "board/si12t_touch.hpp"
#include "conversation/gemini_live_client.hpp"
#include "conversation/openai_realtime_client.hpp"
#include "utf8.hpp"
#include "wifi_sta.hpp"

#include <jtts/jtts.hpp>

namespace stackchan::app {

namespace {

namespace conv = stackchan::conversation;

constexpr const char* kTag = "conv-task";

// Audio rates depend on the selected provider:
//   OpenAI Realtime: µ-law @ 8 kHz both directions (encoded inside the
//                    client, this task just deals in PCM16).
//   Gemini Live:     PCM16 @ 16 kHz uplink, PCM16 @ 24 kHz downlink.
// The values used here are PCM16 sample rates the client expects/produces;
// any companding happens inside the ConversationService impl.
constexpr std::size_t kMaxMicChunkSamples = 640; // worst case: 40 ms @ 16 kHz
constexpr std::uint32_t kEnvelopeStepMs = 16;

// Playback ring: M5.Speaker.playRaw references the buffer (no copy) and its
// resampler reads it sample-by-sample. If that buffer is in PSRAM it contends
// with the render task's 30 fps sprite traffic and the I2S DMA underruns
// (choppy playback). So the reply is accumulated in PSRAM but played back in
// segments copied into this small internal-RAM ring — the speaker only ever
// reads from fast SRAM. 3 buffers: M5.Speaker holds 2, we always have 1 free.
constexpr std::size_t kSegmentSamples = 4096;  // ~512 ms per segment at 8 kHz
constexpr std::size_t kSegmentBuffers = 3;
constexpr int kSpeakerChannel = 0;

// Streaming playback: start speaking once this many ms of reply audio has
// been buffered, rather than waiting for the whole reply. Jitter margin
// against network hiccups. Scaled to actual speaker_sample_rate_ at use.
constexpr std::uint32_t kJitterBufferMs = 300;

// Mic / speaker I2S handoff settle time (matches the existing audio code).
constexpr TickType_t kI2sSettle = pdMS_TO_TICKS(20);

// Recover from a stuck "Thinking" (no audio, no tool follow-up) after this long.
constexpr std::uint32_t kThinkingTimeoutMs = 15000;

// Local half-duplex state machine. Distinct from conv::ConversationState
// (which tracks the protocol) because we hold "Speaking" until the speaker
// has physically drained, independent of when response.done arrives.
enum class Local : std::uint8_t {
    Init,       // waiting for the session to reach Listening
    Listening,  // mic streaming up
    Thinking,   // speech_stopped seen; accumulating the reply
    Speaking,   // playing the reply through the speaker
    Yielded,    // I2S handed off to BLE audio streamer (mic + speaker ended)
};

const char* kInstructions =
    "あなたは「スタックチャン」という小さな卓上ロボットです。M5Stack CoreS3 で動いています。"
    "フレンドリーで元気いっぱい、少し子供っぽい口調で、短く返事をします。ユーザーとは日本語で会話してください。"
    "顔の表情と首の向きを変えられます。気持ちに合わせて set_expression や set_head_pose ツールを使ってください。"
    "また speak_katakoto ツールでロボット風のカタコト声を出すこともできます。"
    "ものまね・効果音・繰り返しなど演出的に使ってください（ツール呼び出しのターンでは普通の声で続けて喋らなくて構いません）。";

conv::ToolDefinition make_set_expression_tool()
{
    return conv::ToolDefinition{
        .name = "set_expression",
        .description = "スタックチャンの顔の表情を変える。感情を表現したいときに使う。",
        .parameters_json =
            R"({"type":"object","properties":{"expression":{"type":"string",)"
            R"("enum":["neutral","happy","sad","angry","doubt","sleepy"]}},"required":["expression"]})",
    };
}

conv::ToolDefinition make_set_head_pose_tool()
{
    return conv::ToolDefinition{
        .name = "set_head_pose",
        .description = "スタックチャンの首の向きを変える。yaw は左右(-40〜40度)、pitch は上下(-10〜25度)。",
        .parameters_json =
            R"({"type":"object","properties":{)"
            R"("yaw_deg":{"type":"number"},"pitch_deg":{"type":"number"}},"required":["yaw_deg","pitch_deg"]})",
    };
}

conv::ToolDefinition make_speak_katakoto_tool()
{
    return conv::ToolDefinition{
        .name = "speak_katakoto",
        .description =
            "ロボット風のカタコト声で短いフレーズを発話する。"
            "kana にはひらがな・カタカナ・長音『ー』・促音『っ』・空白のみを指定する（漢字は不可）。"
            "例: \"ぴこーん\" / \"こんにちわー\" / \"がんばるぞー\"。",
        .parameters_json =
            R"({"type":"object","properties":{)"
            R"("kana":{"type":"string","maxLength":48}},"required":["kana"]})",
    };
}

std::optional<avatar::Expression> parse_expression(const char* name)
{
    if (name == nullptr) {
        return std::nullopt;
    }
    if (std::strcmp(name, "neutral") == 0) return avatar::Expression::Neutral;
    if (std::strcmp(name, "happy") == 0) return avatar::Expression::Happy;
    if (std::strcmp(name, "sad") == 0) return avatar::Expression::Sad;
    if (std::strcmp(name, "angry") == 0) return avatar::Expression::Angry;
    if (std::strcmp(name, "doubt") == 0) return avatar::Expression::Doubt;
    if (std::strcmp(name, "sleepy") == 0) return avatar::Expression::Sleepy;
    return std::nullopt;
}

std::uint32_t now_ms()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

// Owns the conversation; one instance per task.
class Coordinator {
public:
    Coordinator(SharedState& state, const char* api_key, config::Provider provider,
                board::Si12tTouch* touch)
        : state_{state}, api_key_{api_key != nullptr ? api_key : ""},
          provider_{provider}, touch_{touch}
    {
        // Per-provider audio rates. The OpenAI client further compands its
        // 8 kHz PCM16 into µ-law on the wire; Gemini sends raw PCM16.
        if (provider_ == config::Provider::Gemini) {
            mic_sample_rate_ = 16000;
            speaker_sample_rate_ = 24000;
        } else {
            mic_sample_rate_ = 8000;
            speaker_sample_rate_ = 8000;
        }
        mic_chunk_samples_ = mic_sample_rate_ * 40 / 1000;        // 40 ms per chunk
        jitter_buffer_samples_ = speaker_sample_rate_ * kJitterBufferMs / 1000u;
    }

    void run()
    {
        if (api_key_.empty()) {
            ESP_LOGW(kTag, "API key empty for selected provider — conversation disabled");
            vTaskDelete(nullptr);
            return;
        }

        ESP_LOGI(kTag, "waiting for Wi-Fi...");
        while (!wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGI(kTag, "Wi-Fi up, starting %s conversation",
                 provider_ == config::Provider::Gemini ? "Gemini" : "OpenAI");

        event_queue_ = xQueueCreate(32, sizeof(conv::ConversationEvent*));
        mic_buf_[0].resize(mic_chunk_samples_);
        mic_buf_[1].resize(mic_chunk_samples_);

        // Segment ring must live in internal RAM (see kSegmentSamples comment).
        // The Coordinator object itself is heap-allocated and large enough to
        // land in PSRAM, so these can't just be member arrays.
        for (auto& buf : seg_buf_) {
            buf = static_cast<std::int16_t*>(
                heap_caps_malloc(kSegmentSamples * sizeof(std::int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (buf == nullptr) {
                ESP_LOGE(kTag, "failed to allocate internal-RAM segment buffer");
                vTaskDelete(nullptr);
                return;
            }
        }

        if (provider_ == config::Provider::Gemini) {
            client_ = std::make_unique<conv::GeminiLiveClient>(api_key_);
        } else {
            client_ = std::make_unique<conv::OpenAiRealtimeClient>(api_key_);
        }
        client_->set_event_callback([this](const conv::ConversationEvent& ev) { enqueue_event(ev); });

        if (!connect()) {
            ESP_LOGE(kTag, "initial connect failed; conversation disabled");
            vTaskDelete(nullptr);
            return;
        }

        for (;;) {
            // Full conversation shutdown for BLE audio streaming. Yielding
            // just the mic isn't enough — the live WebSocket + mbedtls
            // session keeps Wi-Fi pumping and squeezes internal RAM,
            // which starves the AAC decoder (observed: error 30 only
            // after Wi-Fi associates). When audio_stream_active fires we
            // stop the client entirely so internal RAM rebounds; the
            // streamer publishes conversation_yielded_i2s once we're
            // fully torn down. On clear we reconnect from scratch.
            if (state_.audio_stream_active.load(std::memory_order_acquire)) {
                if (local_ != Local::Yielded) {
                    ESP_LOGI(kTag, "yielding to BLE audio stream — stopping conversation");
                    M5.Mic.end();
                    M5.Speaker.end();
                    state_.mouth_open.store(0.0f, std::memory_order_relaxed);
                    client_->stop();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    flush_events();
                    assistant_pcm_.clear();
                    assistant_text_.clear();
                    set_local(Local::Yielded);
                    state_.conversation_active.store(false, std::memory_order_relaxed);
                    state_.conversation_idle.store(false, std::memory_order_relaxed);
                    state_.conversation_yielded_i2s.store(true, std::memory_order_release);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (local_ == Local::Yielded) {
                ESP_LOGI(kTag, "BLE audio done — restarting conversation");
                state_.conversation_yielded_i2s.store(false, std::memory_order_release);
                if (!connect()) {
                    ESP_LOGW(kTag, "reconnect after audio stream failed; retrying in 5 s");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
                // connect() leaves us in Local::Init waiting for the
                // session-ready event; outer loop will drain_events()
                // and the existing handler progresses us to Listening.
            }
            drain_events();
            service_state();
        }
    }

private:
    // ---- event bridge: WS task -> this task --------------------------------

    void enqueue_event(const conv::ConversationEvent& ev)
    {
        auto* copy = new conv::ConversationEvent(ev);
        if (xQueueSend(event_queue_, &copy, 0) != pdTRUE) {
            ESP_LOGW(kTag, "event queue full, dropping %d", static_cast<int>(ev.type));
            delete copy;
        }
    }

    void drain_events()
    {
        conv::ConversationEvent* ev = nullptr;
        while (xQueueReceive(event_queue_, &ev, 0) == pdTRUE) {
            handle_event(*ev);
            delete ev;
        }
    }

    // Discard whatever's in the event queue without handling. Used by the
    // recovery path to drop stale events emitted by the WebSocket client we
    // just shut down — otherwise the freshly-connected new client gets
    // immediately knocked over by a left-over DISCONNECTED from the old one.
    void flush_events()
    {
        conv::ConversationEvent* ev = nullptr;
        std::size_t dropped = 0;
        while (xQueueReceive(event_queue_, &ev, 0) == pdTRUE) {
            delete ev;
            ++dropped;
        }
        if (dropped > 0) {
            ESP_LOGD(kTag, "flushed %u stale events", static_cast<unsigned>(dropped));
        }
    }

    // ---- session lifecycle -------------------------------------------------

    bool connect()
    {
        conv::ConversationConfig cfg{};
        cfg.instructions = kInstructions;
        if (provider_ == config::Provider::Gemini) {
            // Gemini Live model + voice. The model is namespaced as
            // "models/..."; the client prepends that for us when missing.
            // gemini-2.0-flash-live-001 was deprecated; the native-audio
            // preview is what's actually live on the Developer API right now.
            cfg.model = "gemini-2.5-flash-native-audio-preview-12-2025";
            cfg.voice = "Aoede"; // pre-built voice name; OK to leave empty
        } else {
            cfg.model = CONFIG_STACKCHAN_OPENAI_REALTIME_MODEL;
            cfg.voice = CONFIG_STACKCHAN_OPENAI_VOICE;
        }
        cfg.input_sample_rate_hz = mic_sample_rate_;
        cfg.output_sample_rate_hz = speaker_sample_rate_;
        cfg.tools.push_back(make_set_expression_tool());
        cfg.tools.push_back(make_set_head_pose_tool());
        cfg.tools.push_back(make_speak_katakoto_tool());
        config_ = cfg;

        set_local(Local::Init);
        auto r = client_->start(config_);
        if (!r) {
            ESP_LOGE(kTag, "client start failed: %d", static_cast<int>(r.error()));
            return false;
        }
        return true;
    }

    void recover_after_error()
    {
        ESP_LOGW(kTag, "recovering conversation session");
        if (local_ == Local::Speaking) {
            M5.Speaker.end();
            vTaskDelay(kI2sSettle);
        }
        state_.mouth_open.store(0.0f, std::memory_order_relaxed);
        client_->stop();
        // The shutdown fires WEBSOCKET_EVENT_DISCONNECTED (and sometimes a
        // trailing ERROR) which the trampoline turns into transport-error
        // events. We sit out the WS task's teardown, then flush the queue —
        // otherwise the next drain_events() picks up that disconnect and
        // treats it as a fresh failure of the new client, looping us back
        // into recovery.
        vTaskDelay(pdMS_TO_TICKS(2000));
        flush_events();
        assistant_pcm_.clear();
        assistant_text_.clear();
        if (!connect()) {
            ESP_LOGE(kTag, "reconnect failed; retrying in 5 s");
            vTaskDelay(pdMS_TO_TICKS(5000));
            flush_events();
            connect();
        }
    }

    // ---- per-state servicing ----------------------------------------------

    // Single point of truth for the local state — also publishes "idle" so
    // demo_loop knows when it may run its idle behaviours (head poses,
    // nadenade) without fighting an in-progress reply.
    void set_local(Local s)
    {
        local_ = s;
        state_.conversation_idle.store(s == Local::Listening, std::memory_order_relaxed);
    }

    void service_state()
    {
        switch (local_) {
        case Local::Init:
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        case Local::Listening:
            service_mic();
            break;
        case Local::Thinking:
            if (now_ms() - thinking_since_ms_ > kThinkingTimeoutMs) {
                ESP_LOGW(kTag, "thinking timed out, returning to listening");
                enter_listening();
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            break;
        case Local::Speaking:
            service_playback();
            break;
        case Local::Yielded:
            // Handled at the top of run() — we should never actually
            // dispatch on this state, but the compiler insists.
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }

    // Stream the reply out through the internal-RAM segment ring. assistant_pcm_
    // may still be growing (streaming playback) — feed M5.Speaker while it has
    // a free slot (2-deep queue) and unplayed samples exist. Copying a segment
    // is a fast bulk memcpy, unlike the speaker's sample-by-sample resampler
    // read. Non-blocking so mouth-sync and barge-in polling keep ticking.
    void service_playback()
    {
        // Barge-in: a touch on the head stops the reply and returns to
        // listening. (The mic is physically off while speaking, so this is
        // the only way to interrupt on the half-duplex CoreS3 hardware.)
        // Use firmly_touched() so we don't barge in on a stray Level-1
        // RFI blip — those happen often enough to clobber every reply.
        if (touch_ != nullptr && touch_->read().firmly_touched()) {
            barge_in();
            return;
        }

        while (seg_pos_ < assistant_pcm_.size() &&
               M5.Speaker.isPlaying(kSpeakerChannel) < kSegmentBuffers - 1) {
            const std::size_t n = std::min(kSegmentSamples, assistant_pcm_.size() - seg_pos_);
            std::memcpy(seg_buf_[seg_next_], assistant_pcm_.data() + seg_pos_, n * sizeof(std::int16_t));
            M5.Speaker.playRaw(seg_buf_[seg_next_], n, speaker_sample_rate_, /*stereo=*/false,
                               /*repeat=*/1, kSpeakerChannel, /*stop_current_sound=*/false);
            seg_pos_ += n;
            seg_next_ = (seg_next_ + 1) % kSegmentBuffers;
        }

        update_mouth();

        // Done only once every chunk has arrived, every sample has been queued,
        // and the speaker has physically drained.
        if (audio_complete_ && seg_pos_ >= assistant_pcm_.size() &&
            M5.Speaker.isPlaying(kSpeakerChannel) == 0) {
            finish_speaking();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    void barge_in()
    {
        ESP_LOGI(kTag, "barge-in: user touched the head, interrupting reply");
        M5.Speaker.stop();
        (void)client_->cancel_response();
        state_.set_balloon_text("はいはい？", /*hold_ms=*/1500);
        enter_listening();
    }

    // Double-buffered mic streaming. M5.Mic keeps a 2-deep queue; when one
    // slot frees, push that chunk upstream and re-queue it.
    void service_mic()
    {
        if (M5.Mic.isRecording() < 2) {
            const auto& buf = mic_buf_[mic_read_];
            (void)client_->push_audio(std::span<const std::int16_t>{buf.data(), buf.size()});
            M5.Mic.record(mic_buf_[mic_read_].data(), mic_buf_[mic_read_].size(), mic_sample_rate_, /*stereo=*/false);
            mic_read_ ^= 1;
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    void enter_listening()
    {
        M5.Speaker.end();
        vTaskDelay(kI2sSettle);
        state_.mouth_open.store(0.0f, std::memory_order_relaxed);
        assistant_pcm_.clear();
        // Reserve ahead so the streaming-playback inserts don't keep
        // reallocating the PSRAM buffer as the reply grows.
        assistant_pcm_.reserve(speaker_sample_rate_ * 20); // ~20 s headroom
        assistant_text_.clear();
        audio_complete_ = false;
        // Prime the 2-deep mic queue.
        M5.Mic.record(mic_buf_[0].data(), mic_buf_[0].size(), mic_sample_rate_, /*stereo=*/false);
        M5.Mic.record(mic_buf_[1].data(), mic_buf_[1].size(), mic_sample_rate_, /*stereo=*/false);
        mic_read_ = 0;
        set_local(Local::Listening);
    }

    // Switch the I2S bus to the speaker and begin streaming playback. Called
    // as soon as the jitter buffer fills (or on AssistantAudioDone for replies
    // shorter than the jitter buffer) — assistant_pcm_ may still be growing.
    void start_speaking()
    {
        M5.Mic.end();
        vTaskDelay(kI2sSettle);
        seg_pos_ = 0;
        seg_next_ = 0;
        playback_start_ms_ = now_ms();
        set_local(Local::Speaking);
        ESP_LOGI(kTag, "speaking (streaming)");
    }

    // Mouth-open is derived on the fly from the 16 ms window of reply audio
    // currently being played — no precomputed envelope, so it works while
    // assistant_pcm_ is still being streamed in.
    void update_mouth()
    {
        const std::size_t window = speaker_sample_rate_ * kEnvelopeStepMs / 1000u;
        const std::uint32_t elapsed = now_ms() - playback_start_ms_;
        const std::size_t begin = (elapsed / kEnvelopeStepMs) * window;
        if (begin + window > assistant_pcm_.size()) {
            state_.mouth_open.store(0.0f, std::memory_order_relaxed);
            return;
        }
        std::int32_t peak = 0;
        for (std::size_t i = begin; i < begin + window; ++i) {
            peak = std::max(peak, std::abs(static_cast<std::int32_t>(assistant_pcm_[i])));
        }
        state_.mouth_open.store(static_cast<float>(peak) / 32767.0f, std::memory_order_relaxed);
    }

    void finish_speaking()
    {
        M5.Speaker.end();
        vTaskDelay(kI2sSettle);
        state_.mouth_open.store(0.0f, std::memory_order_relaxed);
        enter_listening();
    }

    // ---- event handling ----------------------------------------------------

    void handle_event(const conv::ConversationEvent& ev)
    {
        switch (ev.type) {
        case conv::ConversationEventType::StateChanged:
            // The protocol reaching Listening for the first time means the
            // session is live. Later protocol transitions are ignored — the
            // local state machine owns mic/speaker timing.
            if (local_ == Local::Init && ev.state == conv::ConversationState::Listening) {
                ESP_LOGI(kTag, "session ready");
                state_.conversation_active.store(true, std::memory_order_relaxed);
                enter_listening();
            }
            break;

        case conv::ConversationEventType::SpeechStarted:
            ESP_LOGI(kTag, "user speech started");
            break;

        case conv::ConversationEventType::SpeechStopped:
            ESP_LOGI(kTag, "user speech stopped");
            if (local_ == Local::Listening) {
                set_local(Local::Thinking);
                thinking_since_ms_ = now_ms();
            }
            break;

        case conv::ConversationEventType::UserTranscript:
            ESP_LOGI(kTag, "user: %s", ev.text.c_str());
            if (!ev.text.empty()) {
                state_.set_balloon_text(ev.text, /*hold_ms=*/2500);
            }
            break;

        case conv::ConversationEventType::AssistantTextDelta:
            assistant_text_ += ev.text;
            break;

        case conv::ConversationEventType::AssistantTextDone:
            if (!ev.text.empty()) {
                assistant_text_ = ev.text;
            }
            ESP_LOGI(kTag, "assistant: %s", assistant_text_.c_str());
            if (!assistant_text_.empty()) {
                state_.set_balloon_text(assistant_text_, /*hold_ms=*/0);
            }
            break;

        case conv::ConversationEventType::AssistantAudioChunk:
            // Ignore late chunks for a turn we already abandoned (barge-in).
            if (ev.audio && (local_ == Local::Thinking || local_ == Local::Speaking)) {
                assistant_pcm_.insert(assistant_pcm_.end(), ev.audio->begin(), ev.audio->end());
                // Streaming: start playback as soon as the jitter buffer fills,
                // rather than waiting for the whole reply.
                if (local_ == Local::Thinking && assistant_pcm_.size() >= jitter_buffer_samples_) {
                    tool_pending_ = false;
                    start_speaking();
                }
            }
            break;

        case conv::ConversationEventType::AssistantAudioDone:
            audio_complete_ = true;
            // Reply shorter than the jitter buffer — it never tripped the
            // streaming start, so begin playback now.
            if (local_ == Local::Thinking && !assistant_pcm_.empty()) {
                tool_pending_ = false;
                start_speaking();
            }
            break;

        case conv::ConversationEventType::ToolCallRequested:
            if (ev.tool_call) {
                dispatch_tool(*ev.tool_call);
            }
            break;

        case conv::ConversationEventType::ResponseDone:
            // The response that carried a tool call ends here, but a follow-up
            // response (the model's actual reply) is still coming after we
            // submit the tool result — stay in Thinking and wait for it.
            if (tool_pending_) {
                tool_pending_ = false;
                thinking_since_ms_ = now_ms();
            } else if (local_ == Local::Thinking && assistant_pcm_.empty()) {
                // Text-only or empty turn: nothing will switch us to Speaking.
                enter_listening();
            }
            break;

        case conv::ConversationEventType::Error: {
            // Transport errors (disconnect / handshake) are fatal — the
            // session is gone and must be rebuilt. Server-side errors (bad
            // request, a no-op cancellation, rate-limit notices, …) leave the
            // session alive, so just log them and carry on.
            const bool transport_error = ev.error == conv::ConversationError::NotConnected ||
                                         ev.error == conv::ConversationError::TransportInit;
            if (transport_error) {
                ESP_LOGE(kTag, "transport error: %s — reconnecting", ev.text.c_str());
                state_.set_balloon_text("接続エラー", /*hold_ms=*/3000);
                recover_after_error();
            } else {
                ESP_LOGW(kTag, "server error (non-fatal): %s", ev.text.c_str());
            }
            break;
        }
        }
    }

    // ---- tool dispatch -----------------------------------------------------

    void dispatch_tool(const conv::ToolCall& call)
    {
        ESP_LOGI(kTag, "tool call: %s args=%s", call.name.c_str(), call.arguments_json.c_str());
        std::string result = R"({"ok":false,"error":"unknown tool"})";

        if (call.name == "set_expression") {
            result = handle_set_expression(call.arguments_json);
        } else if (call.name == "set_head_pose") {
            result = handle_set_head_pose(call.arguments_json);
        } else if (call.name == "speak_katakoto") {
            result = handle_speak_katakoto(call.arguments_json);
        }

        // The model will issue a follow-up response after we return the tool
        // result; mark it pending so ResponseDone for the tool-call response
        // doesn't drop us back to Listening prematurely.
        tool_pending_ = true;
        auto r = client_->submit_tool_result(call.call_id, result);
        if (!r) {
            ESP_LOGE(kTag, "submit_tool_result failed: %d", static_cast<int>(r.error()));
        }
    }

    std::string handle_set_expression(const std::string& args)
    {
        cJSON* root = cJSON_Parse(args.c_str());
        if (root == nullptr) {
            return R"({"ok":false,"error":"bad arguments"})";
        }
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, "expression");
        const char* name = cJSON_IsString(item) ? item->valuestring : nullptr;
        const auto expr = parse_expression(name);
        std::string result;
        if (expr) {
            state_.expression.store(static_cast<int>(*expr), std::memory_order_relaxed);
            result = R"({"ok":true})";
        } else {
            result = R"({"ok":false,"error":"unknown expression"})";
        }
        cJSON_Delete(root);
        return result;
    }

    // Synthesise the kana phrase through jtts and play it through the speaker.
    // Blocks the conversation task until playback drains, so the tool result
    // is sent back only after the audible utterance finishes — this avoids
    // overlapping with any follow-up reply the model might produce. The PCM
    // is generated into a PSRAM vector then streamed through seg_buf_ (the
    // same internal-RAM ring used by reply playback) to dodge PSRAM contention.
    std::string handle_speak_katakoto(const std::string& args)
    {
        cJSON* root = cJSON_Parse(args.c_str());
        if (root == nullptr) {
            return R"({"ok":false,"error":"bad arguments"})";
        }
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, "kana");
        std::string kana_utf8;
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            kana_utf8 = item->valuestring;
        }
        cJSON_Delete(root);
        if (kana_utf8.empty()) {
            return R"({"ok":false,"error":"kana required"})";
        }

        const std::u32string kana = decode_utf8(kana_utf8);
        if (kana.empty()) {
            return R"({"ok":false,"error":"invalid utf8"})";
        }

        // Robotic-katakoto preset: low monotone male voice, slightly halting
        // mora pace. Deliberately different from the assistant's normal voice
        // so the user clearly hears it as a separate "mode".
        constexpr std::uint32_t kKatakotoRate = 16000;
        stackchan::jtts::Options opt;
        opt.voice = stackchan::jtts::Voice::Male;
        opt.f0_hz = 140.0f;
        opt.mora_ms = 140.0f;
        opt.gain = 0.8f;
        opt.sample_rate_hz = kKatakotoRate;

        std::vector<std::int16_t> pcm;
        auto r = stackchan::jtts::synthesize(kana, pcm, opt);
        if (!r) {
            ESP_LOGW(kTag, "jtts synthesize failed: %s",
                     stackchan::jtts::to_string(r.error()));
            return R"({"ok":false,"error":"synthesize failed"})";
        }
        if (pcm.empty()) {
            return R"({"ok":true,"warning":"empty audio"})";
        }

        // I2S handoff: mic → speaker. We may be in Listening (mic primed) or
        // Thinking (mic primed but not being drained); either way the bus
        // needs to belong to the speaker before playRaw.
        M5.Mic.end();
        vTaskDelay(kI2sSettle);

        // Pre-compute peak envelope so the avatar mouth opens in sync with the
        // utterance even though we're not running the normal service_playback.
        const std::size_t env_window = kKatakotoRate * kEnvelopeStepMs / 1000u;
        std::vector<float> envelope;
        if (env_window > 0) {
            envelope.reserve((pcm.size() + env_window - 1) / env_window);
            for (std::size_t i = 0; i < pcm.size(); i += env_window) {
                std::int32_t peak = 0;
                const std::size_t end = std::min(i + env_window, pcm.size());
                for (std::size_t j = i; j < end; ++j) {
                    peak = std::max(peak, std::abs(static_cast<std::int32_t>(pcm[j])));
                }
                envelope.push_back(static_cast<float>(peak) / 32767.0f);
            }
        }

        auto update_mouth_from_envelope = [&](std::uint32_t elapsed_ms) {
            const std::size_t idx = elapsed_ms / kEnvelopeStepMs;
            const float open = idx < envelope.size() ? envelope[idx] : 0.0f;
            state_.mouth_open.store(open, std::memory_order_relaxed);
        };

        const std::uint32_t start_ms = now_ms();
        std::size_t pos = 0;
        std::size_t next = 0;
        while (pos < pcm.size()) {
            if (M5.Speaker.isPlaying(kSpeakerChannel) < kSegmentBuffers - 1) {
                const std::size_t n = std::min(kSegmentSamples, pcm.size() - pos);
                std::memcpy(seg_buf_[next], pcm.data() + pos, n * sizeof(std::int16_t));
                M5.Speaker.playRaw(seg_buf_[next], n, kKatakotoRate, /*stereo=*/false,
                                   /*repeat=*/1, kSpeakerChannel, /*stop_current_sound=*/false);
                pos += n;
                next = (next + 1) % kSegmentBuffers;
            }
            update_mouth_from_envelope(now_ms() - start_ms);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        while (M5.Speaker.isPlaying(kSpeakerChannel) > 0) {
            update_mouth_from_envelope(now_ms() - start_ms);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        M5.Speaker.end();
        vTaskDelay(kI2sSettle);
        state_.mouth_open.store(0.0f, std::memory_order_relaxed);
        return R"({"ok":true})";
    }

    std::string handle_set_head_pose(const std::string& args)
    {
        cJSON* root = cJSON_Parse(args.c_str());
        if (root == nullptr) {
            return R"({"ok":false,"error":"bad arguments"})";
        }
        const cJSON* yaw = cJSON_GetObjectItemCaseSensitive(root, "yaw_deg");
        const cJSON* pitch = cJSON_GetObjectItemCaseSensitive(root, "pitch_deg");
        std::string result;
        if (cJSON_IsNumber(yaw) && cJSON_IsNumber(pitch)) {
            const float yaw_deg = std::clamp(static_cast<float>(yaw->valuedouble), -40.0f, 40.0f);
            const float pitch_deg = std::clamp(static_cast<float>(pitch->valuedouble), -10.0f, 25.0f);
            state_.target_yaw_deg.store(yaw_deg, std::memory_order_relaxed);
            state_.target_pitch_deg.store(pitch_deg, std::memory_order_relaxed);
            result = R"({"ok":true})";
        } else {
            result = R"({"ok":false,"error":"yaw_deg and pitch_deg required"})";
        }
        cJSON_Delete(root);
        return result;
    }

    // ---- members -----------------------------------------------------------

    SharedState& state_;
    std::string api_key_;
    config::Provider provider_;
    board::Si12tTouch* touch_; // top touch sensor for barge-in (may be null)
    conv::ConversationConfig config_{};
    std::unique_ptr<conv::ConversationService> client_;
    QueueHandle_t event_queue_{nullptr};

    // Per-provider audio rates set in the constructor.
    std::uint32_t mic_sample_rate_{8000};
    std::uint32_t speaker_sample_rate_{8000};
    std::size_t mic_chunk_samples_{320};
    std::size_t jitter_buffer_samples_{2400};

    Local local_{Local::Init};
    std::uint32_t thinking_since_ms_{0};
    bool tool_pending_{false};

    std::array<std::vector<std::int16_t>, 2> mic_buf_{};
    int mic_read_{0};

    std::vector<std::int16_t> assistant_pcm_;          // reply audio, in PSRAM (grows while streaming)
    bool audio_complete_{false};                       // every AssistantAudioChunk has arrived
    std::array<std::int16_t*, kSegmentBuffers> seg_buf_{}; // internal-RAM playback ring
    std::size_t seg_pos_{0};                           // next sample in assistant_pcm_ to play
    std::size_t seg_next_{0};                          // next ring slot to write
    std::uint32_t playback_start_ms_{0};
    std::string assistant_text_;
};

void conversation_task_entry(void* arg)
{
    auto& args = *static_cast<ConversationTaskArgs*>(arg);
    auto* coordinator = new Coordinator(*args.state, args.api_key, args.provider, args.touch);
    coordinator->run();
    // run() only returns by deleting the task; keep the object alive regardless.
    vTaskDelete(nullptr);
}

} // namespace

void start_conversation_task(ConversationTaskArgs& args)
{
    xTaskCreatePinnedToCore(conversation_task_entry, "conversation", 8192, &args, 5, nullptr, 0);
}

} // namespace stackchan::app
