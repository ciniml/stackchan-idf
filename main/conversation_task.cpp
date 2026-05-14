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
#include "conversation/openai_realtime_client.hpp"
#include "wifi_provisioning.hpp"

namespace stackchan::app {

namespace {

namespace conv = stackchan::conversation;

constexpr const char* kTag = "conv-task";

// Audio rates. Mic uplink stays pcm16 @ 24 kHz; the assistant reply comes back
// as G.711 µ-law @ 8 kHz (the OpenAI client decodes µ-law -> PCM16 internally,
// so this task always sees PCM16 — just at 8 kHz for playback).
constexpr std::uint32_t kMicSampleRate = 24000;
constexpr std::uint32_t kSpeakerSampleRate = 8000;
constexpr std::size_t kMicChunkSamples = 960;  // 40 ms per mic chunk
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
};

const char* kInstructions =
    "あなたは「スタックチャン」という小さな卓上ロボットです。M5Stack CoreS3 で動いています。"
    "フレンドリーで元気いっぱい、少し子供っぽい口調で、短く返事をします。ユーザーとは日本語で会話してください。"
    "顔の表情と首の向きを変えられます。気持ちに合わせて set_expression や set_head_pose ツールを使ってください。";

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
    Coordinator(SharedState& state, const char* api_key)
        : state_{state}, api_key_{api_key != nullptr ? api_key : ""}
    {
    }

    void run()
    {
        if (api_key_.empty()) {
            ESP_LOGW(kTag, "OPENAI_API_KEY is empty — conversation disabled, demo mode continues");
            vTaskDelete(nullptr);
            return;
        }

        ESP_LOGI(kTag, "waiting for Wi-Fi...");
        while (!wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGI(kTag, "Wi-Fi up, starting conversation");

        event_queue_ = xQueueCreate(32, sizeof(conv::ConversationEvent*));
        mic_buf_[0].resize(kMicChunkSamples);
        mic_buf_[1].resize(kMicChunkSamples);

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

        client_ = std::make_unique<conv::OpenAiRealtimeClient>(api_key_);
        client_->set_event_callback([this](const conv::ConversationEvent& ev) { enqueue_event(ev); });

        if (!connect()) {
            ESP_LOGE(kTag, "initial connect failed; conversation disabled");
            vTaskDelete(nullptr);
            return;
        }

        for (;;) {
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

    // ---- session lifecycle -------------------------------------------------

    bool connect()
    {
        conv::ConversationConfig cfg{};
        cfg.instructions = kInstructions;
        cfg.voice = CONFIG_STACKCHAN_OPENAI_VOICE;
        cfg.model = CONFIG_STACKCHAN_OPENAI_REALTIME_MODEL;
        cfg.input_sample_rate_hz = kMicSampleRate;
        cfg.output_sample_rate_hz = kSpeakerSampleRate; // 8 kHz selects g711_ulaw on the wire
        cfg.tools.push_back(make_set_expression_tool());
        cfg.tools.push_back(make_set_head_pose_tool());
        config_ = cfg;

        local_ = Local::Init;
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
        vTaskDelay(pdMS_TO_TICKS(2000));
        assistant_pcm_.clear();
        assistant_text_.clear();
        if (!connect()) {
            ESP_LOGE(kTag, "reconnect failed; retrying in 5 s");
            vTaskDelay(pdMS_TO_TICKS(5000));
            connect();
        }
    }

    // ---- per-state servicing ----------------------------------------------

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
        }
    }

    // Stream the PSRAM reply out through the internal-RAM segment ring. Feed
    // M5.Speaker while it has a free slot (it keeps a 2-deep queue); copying a
    // segment is a fast bulk memcpy, unlike the speaker's sample-by-sample
    // resampler read. Non-blocking so mouth-sync keeps ticking.
    void service_playback()
    {
        while (seg_pos_ < assistant_pcm_.size() &&
               M5.Speaker.isPlaying(kSpeakerChannel) < kSegmentBuffers - 1) {
            const std::size_t n = std::min(kSegmentSamples, assistant_pcm_.size() - seg_pos_);
            std::memcpy(seg_buf_[seg_next_], assistant_pcm_.data() + seg_pos_, n * sizeof(std::int16_t));
            M5.Speaker.playRaw(seg_buf_[seg_next_], n, kSpeakerSampleRate, /*stereo=*/false,
                               /*repeat=*/1, kSpeakerChannel, /*stop_current_sound=*/false);
            seg_pos_ += n;
            seg_next_ = (seg_next_ + 1) % kSegmentBuffers;
        }

        update_mouth();

        if (seg_pos_ >= assistant_pcm_.size() && M5.Speaker.isPlaying(kSpeakerChannel) == 0) {
            finish_speaking();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Double-buffered mic streaming. M5.Mic keeps a 2-deep queue; when one
    // slot frees, push that chunk upstream and re-queue it.
    void service_mic()
    {
        if (M5.Mic.isRecording() < 2) {
            const auto& buf = mic_buf_[mic_read_];
            (void)client_->push_audio(std::span<const std::int16_t>{buf.data(), buf.size()});
            M5.Mic.record(mic_buf_[mic_read_].data(), mic_buf_[mic_read_].size(), kMicSampleRate, /*stereo=*/false);
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
        assistant_text_.clear();
        // Prime the 2-deep mic queue.
        M5.Mic.record(mic_buf_[0].data(), mic_buf_[0].size(), kMicSampleRate, /*stereo=*/false);
        M5.Mic.record(mic_buf_[1].data(), mic_buf_[1].size(), kMicSampleRate, /*stereo=*/false);
        mic_read_ = 0;
        local_ = Local::Listening;
    }

    void start_speaking()
    {
        M5.Mic.end();
        vTaskDelay(kI2sSettle);

        // Re-derive a 16 ms-window peak envelope so the avatar mouth tracks
        // the streamed reply (same approach as Speech::current_mouth_open).
        const std::size_t window = kSpeakerSampleRate * kEnvelopeStepMs / 1000u;
        const std::size_t windows = (assistant_pcm_.size() + window - 1) / std::max<std::size_t>(window, 1);
        envelope_.assign(windows, 0.0f);
        for (std::size_t w = 0; w < windows; ++w) {
            const std::size_t begin = w * window;
            const std::size_t end = std::min(begin + window, assistant_pcm_.size());
            std::int32_t peak = 0;
            for (std::size_t i = begin; i < end; ++i) {
                peak = std::max(peak, std::abs(static_cast<std::int32_t>(assistant_pcm_[i])));
            }
            envelope_[w] = static_cast<float>(peak) / 32767.0f;
        }

        playback_start_ms_ = now_ms();
        playback_duration_ms_ =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(assistant_pcm_.size()) * 1000u / kSpeakerSampleRate);

        // The reply stays in PSRAM; service_state() streams it out segment by
        // segment through the internal-RAM ring so the speaker never reads
        // PSRAM directly. Playback itself starts on the next service tick.
        seg_pos_ = 0;
        seg_next_ = 0;
        local_ = Local::Speaking;
        ESP_LOGI(kTag, "speaking %u ms of reply", static_cast<unsigned>(playback_duration_ms_));
    }

    void update_mouth()
    {
        const std::uint32_t elapsed = now_ms() - playback_start_ms_;
        if (elapsed >= playback_duration_ms_ || envelope_.empty()) {
            state_.mouth_open.store(0.0f, std::memory_order_relaxed);
            return;
        }
        const std::size_t idx = elapsed / kEnvelopeStepMs;
        state_.mouth_open.store(idx < envelope_.size() ? envelope_[idx] : 0.0f, std::memory_order_relaxed);
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
            state_.conversation_state.store(static_cast<int>(ev.state), std::memory_order_relaxed);
            break;

        case conv::ConversationEventType::SpeechStarted:
            ESP_LOGI(kTag, "user speech started");
            break;

        case conv::ConversationEventType::SpeechStopped:
            ESP_LOGI(kTag, "user speech stopped");
            if (local_ == Local::Listening) {
                local_ = Local::Thinking;
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
            if (ev.audio) {
                assistant_pcm_.insert(assistant_pcm_.end(), ev.audio->begin(), ev.audio->end());
            }
            break;

        case conv::ConversationEventType::AssistantAudioDone:
            if (!assistant_pcm_.empty() && local_ != Local::Speaking) {
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

        case conv::ConversationEventType::Error:
            ESP_LOGE(kTag, "conversation error: %s", ev.text.c_str());
            state_.set_balloon_text("接続エラー", /*hold_ms=*/3000);
            recover_after_error();
            break;
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
    conv::ConversationConfig config_{};
    std::unique_ptr<conv::OpenAiRealtimeClient> client_;
    QueueHandle_t event_queue_{nullptr};

    Local local_{Local::Init};
    std::uint32_t thinking_since_ms_{0};
    bool tool_pending_{false};

    std::array<std::vector<std::int16_t>, 2> mic_buf_{};
    int mic_read_{0};

    std::vector<std::int16_t> assistant_pcm_;          // full reply, in PSRAM
    std::array<std::int16_t*, kSegmentBuffers> seg_buf_{}; // internal-RAM playback ring
    std::size_t seg_pos_{0};                           // next sample in assistant_pcm_ to play
    std::size_t seg_next_{0};                          // next ring slot to write
    std::vector<float> envelope_;
    std::uint32_t playback_start_ms_{0};
    std::uint32_t playback_duration_ms_{0};
    std::string assistant_text_;
};

void conversation_task_entry(void* arg)
{
    auto& args = *static_cast<ConversationTaskArgs*>(arg);
    auto* coordinator = new Coordinator(*args.state, args.api_key);
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
