// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "conversation/conversation_error.hpp"

namespace stackchan::conversation {

// Generic interface for an AI conversational-response service.
//
// Different backends split the pipeline differently: some expose VAD / noise
// suppression / STT / response generation / TTS as separate stages, while
// others (e.g. the OpenAI Realtime API) run the whole pipeline server-side
// over a single session. This interface hides that difference — the caller
// feeds microphone PCM in and receives a stream of high-level events
// (transcripts, assistant text, assistant audio, tool-call requests, state
// changes) regardless of how the backend is structured internally.

enum class ConversationState : std::uint8_t {
    Idle,
    Connecting,
    Listening,
    Thinking,
    Speaking,
    Error,
};

// A tool/function the model may invoke. `parameters_json` is a raw JSON-schema
// object (as a string) describing the tool's arguments.
struct ToolDefinition {
    std::string name;
    std::string description;
    std::string parameters_json;
};

struct ConversationConfig {
    std::string instructions; // persona / system prompt
    std::string voice;
    std::string model;
    std::uint32_t input_sample_rate_hz{24000};
    std::uint32_t output_sample_rate_hz{24000};
    std::vector<ToolDefinition> tools;
    // Extra HTTP headers to attach to the WebSocket upgrade request, as a
    // newline-separated list of "Name: value" lines. Used for e.g. a
    // Cloudflare Access service token in front of a proxied API. Empty = none.
    std::string extra_headers;
    bool enable_input_transcription{true};
    std::uint32_t vad_silence_ms{500};
    std::uint32_t vad_prefix_padding_ms{300};
    float vad_threshold{0.5f};
};

enum class ConversationEventType : std::uint8_t {
    StateChanged,
    SpeechStarted,
    SpeechStopped,
    UserTranscript,
    AssistantTextDelta,
    AssistantTextDone,
    AssistantAudioChunk,
    AssistantAudioDone,
    // The backend reported an affective state for the current reply (e.g.
    // XiaoZhi's `llm` emotion message). `text` carries the emotion name; the
    // application maps it to an avatar expression. Backends that don't model
    // emotion separately (OpenAI / Gemini drive expression via a tool call)
    // simply never emit this.
    AssistantEmotion,
    ToolCallRequested,
    ResponseDone,
    Error,
    // Backend warned that the current connection will be terminated soon
    // (Gemini Live `goAway` message — currently fires near the 15-min audio
    // session cap, but the server may send it at any time). The application
    // is expected to reconnect proactively; the existing resumption handle
    // is preserved across the reconnect so the conversation continues. The
    // event's `text` field carries the server's timeLeft as a string (e.g.
    // "5s") for diagnostic logging; no special parsing is required because
    // the conv-task just hands off to its standard recovery path which
    // already saves+restores the handle.
    GoingAway,
};

struct ToolCall {
    std::string call_id;
    std::string name;
    std::string arguments_json;
};

// A single observable milestone from the conversation pipeline. A given
// backend emits the subset of event types it can produce. Audio is carried as
// a shared_ptr so the event stays cheap to copy across a FreeRTOS queue — only
// the refcount is copied, not the samples.
struct ConversationEvent {
    ConversationEventType type;
    ConversationState state{ConversationState::Idle};        // for StateChanged
    std::string text;                                        // transcripts / text deltas
    std::shared_ptr<const std::vector<std::int16_t>> audio;  // PCM16 mono chunk
    std::optional<ToolCall> tool_call;
    std::optional<ConversationError> error;
    // `esp_timer_get_time()` at emit, in microseconds. Used by the conv-task
    // to measure event-queue latency (`now_us() - emit_us`) when diagnosing
    // audio pipeline stalls. 0 if the producer didn't set it.
    std::int64_t emit_us{0};
};

class ConversationService {
public:
    using EventCallback = std::function<void(const ConversationEvent&)>;

    virtual ~ConversationService() = default;

    // Register the sink for pipeline events.
    //
    // THREADING: the callback runs in the backend's internal task context
    // (for the OpenAI client, the esp_websocket_client event task). It must be
    // cheap and non-blocking — the recommended pattern is to marshal the event
    // onto a FreeRTOS queue and let an application task do the real work.
    virtual void set_event_callback(EventCallback cb) = 0;

    // Begin a session. Asynchronous: returns once the connection has been
    // kicked off; readiness is signalled later via StateChanged(Listening).
    virtual tl::expected<void, ConversationError> start(const ConversationConfig& config) = 0;

    // Tear down the session.
    virtual void stop() = 0;

    // Feed a chunk of microphone PCM16 (mono, ConversationConfig sample rate).
    virtual tl::expected<void, ConversationError> push_audio(std::span<const std::int16_t> pcm) = 0;

    // Explicitly mark the end of the user's utterance. Needed by client-VAD
    // backends; a documented no-op for backends that use server-side VAD.
    virtual tl::expected<void, ConversationError> commit_audio() = 0;

    // Return the result of a tool call (in response to a ToolCallRequested
    // event) and ask the model to continue.
    virtual tl::expected<void, ConversationError> submit_tool_result(std::string_view call_id,
                                                                     std::string_view output_json) = 0;

    // Interrupt the assistant's in-progress response (barge-in): stop the
    // backend generating/streaming and return the session to listening.
    virtual tl::expected<void, ConversationError> cancel_response() = 0;

    virtual ConversationState state() const = 0;

    // Number of mic-uplink chunks evicted (oldest-dropped) from the client's
    // audio TX queue because the WebSocket sender could not keep up,
    // cumulative since the last start(). Polled by the application at
    // end-of-turn so uplink congestion shows up in the audio-metrics snapshot
    // instead of as per-chunk log spam. Backends without a client-side TX
    // queue keep the default 0.
    virtual std::uint32_t tx_evicted_chunks() const { return 0; }
};

} // namespace stackchan::conversation
