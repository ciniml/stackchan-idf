// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "conversation/openai_realtime_client.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

#include "base64.hpp"

namespace stackchan::conversation {

namespace {

constexpr const char* kTag = "openai-rt";

// wss host + path. The model is appended as a query parameter.
constexpr const char* kRealtimeUriPrefix = "wss://api.openai.com/v1/realtime?model=";

// Hot-path scratch is sized for this many PCM16 samples per push_audio chunk.
// The coordinator streams ~20-40 ms chunks (480-960 samples at 24 kHz); 2048
// leaves generous headroom so the append path never allocates.
constexpr std::size_t kMaxChunkSamples = 2048;

// Wi-Fi RTT is normally <100 ms, but bursts of latency or AP roaming can push
// individual writes past several seconds. With the old 500 ms cap, a single
// blip during a 40-ms mic chunk's send would fail transport_poll_write and
// tear down the whole WebSocket — so the conversation would reconnect mid-
// sentence every time the network burped. 3 s tolerates the typical worst
// case without making the audio path feel laggy when things are healthy.
constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(3000);

// OpenAI Realtime only offers pcm16 at 24 kHz or G.711 at 8 kHz, so the
// requested output sample rate uniquely selects the wire codec.
constexpr std::uint32_t kG711SampleRate = 8000;

// Standard G.711 µ-law decode (Sun algorithm, BIAS = 0x84). One companded
// byte -> one 16-bit PCM sample.
std::int16_t ulaw_to_pcm16(std::uint8_t u_val)
{
    u_val = ~u_val;
    std::int32_t t = ((u_val & 0x0F) << 3) + 0x84;
    t <<= (u_val & 0x70) >> 4;
    return static_cast<std::int16_t>((u_val & 0x80) ? (0x84 - t) : (t - 0x84));
}

} // namespace

class OpenAiRealtimeClient::Impl {
public:
    explicit Impl(std::string api_key) : api_key_{std::move(api_key)} {}

    ~Impl() { teardown(); }

    tl::expected<void, ConversationError> start(const ConversationConfig& config)
    {
        if (client_ != nullptr) {
            return tl::unexpected{ConversationError::InvalidState};
        }
        config_ = config;
        output_is_ulaw_ = (config_.output_sample_rate_hz == kG711SampleRate);

        const std::size_t rx_cap = static_cast<std::size_t>(CONFIG_STACKCHAN_CONV_WS_RX_BUFFER);
        rx_buffer_ = static_cast<char*>(heap_caps_malloc(rx_cap, MALLOC_CAP_SPIRAM));
        if (rx_buffer_ == nullptr) {
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        rx_capacity_ = rx_cap;
        rx_len_ = 0;
        rx_op_code_ = 0;

        const std::size_t b64_cap = base64::encoded_size(kMaxChunkSamples * sizeof(std::int16_t));
        b64_scratch_.assign(b64_cap, '\0');
        json_scratch_.assign(b64_cap + 128, '\0');

        const std::string uri = std::string{kRealtimeUriPrefix} + config_.model;

        esp_websocket_client_config_t cfg{};
        cfg.uri = uri.c_str();
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.buffer_size = 8192;
        cfg.task_stack = 6144;
        cfg.task_prio = 5;
        cfg.disable_auto_reconnect = true;
        cfg.network_timeout_ms = 10000;
        // Active keepalive: client sends a WebSocket PING every 15 s; if no
        // PONG arrives within 25 s the client drops the connection (PONGs
        // are auto-handled by esp_websocket_client). This catches silently-
        // dead TCP links without waiting for the next mic-chunk send to fail.
        cfg.ping_interval_sec = 15;
        cfg.pingpong_timeout_sec = 25;
        // Layer-4 TCP keepalive as a second line of defence — kernel probes
        // notice broken NATs / AP reboots even if the WS ping cycle hasn't
        // come around yet.
        cfg.keep_alive_enable = true;
        cfg.keep_alive_idle = 10;
        cfg.keep_alive_interval = 5;
        cfg.keep_alive_count = 3;

        client_ = esp_websocket_client_init(&cfg);
        if (client_ == nullptr) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }

        const std::string auth = std::string{"Bearer "} + api_key_;
        esp_websocket_client_append_header(client_, "Authorization", auth.c_str());
        // OpenAI Realtime is GA — the OpenAI-Beta: realtime=v1 header is no
        // longer accepted (the server returns "Realtime Beta API is no longer
        // supported").

        esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &Impl::websocket_event_trampoline, this);

        session_updated_ = false;
        pending_tool_call_ = false;
        set_state(ConversationState::Connecting);

        if (esp_websocket_client_start(client_) != ESP_OK) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }
        return {};
    }

    void stop() { teardown(); }

    tl::expected<void, ConversationError> push_audio(std::span<const std::int16_t> pcm)
    {
        if (state_.load(std::memory_order_relaxed) != ConversationState::Listening) {
            // Not an error: the coordinator may still be draining the mic
            // after speech_stopped. Drop silently.
            ESP_LOGD(kTag, "push_audio dropped (state != Listening)");
            return {};
        }
        if (pcm.empty()) {
            return {};
        }

        std::lock_guard lock{send_mutex_};
        if (client_ == nullptr || !esp_websocket_client_is_connected(client_)) {
            return tl::unexpected{ConversationError::NotConnected};
        }

        const auto bytes = std::as_bytes(pcm);
        const std::span<const std::uint8_t> raw{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};

        // Hot path: encode + JSON-wrap into preallocated scratch, no heap.
        const std::size_t needed_b64 = base64::encoded_size(raw.size());
        if (needed_b64 > b64_scratch_.size()) {
            b64_scratch_.assign(needed_b64, '\0');
            json_scratch_.assign(needed_b64 + 128, '\0');
        }
        auto enc = base64::encode_into(raw, b64_scratch_);
        if (!enc) {
            return tl::unexpected{enc.error()};
        }
        const int n = std::snprintf(json_scratch_.data(), json_scratch_.size(),
                                    "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%.*s\"}",
                                    static_cast<int>(*enc), b64_scratch_.data());
        if (n <= 0 || static_cast<std::size_t>(n) >= json_scratch_.size()) {
            return tl::unexpected{ConversationError::SendFailed};
        }
        if (esp_websocket_client_send_text(client_, json_scratch_.data(), n, kSendTimeout) < 0) {
            return tl::unexpected{ConversationError::SendFailed};
        }
        return {};
    }

    tl::expected<void, ConversationError> submit_tool_result(std::string_view call_id, std::string_view output_json)
    {
        std::lock_guard lock{send_mutex_};
        if (client_ == nullptr || !esp_websocket_client_is_connected(client_)) {
            return tl::unexpected{ConversationError::NotConnected};
        }

        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "conversation.item.create");
        cJSON* item = cJSON_AddObjectToObject(root, "item");
        cJSON_AddStringToObject(item, "type", "function_call_output");
        cJSON_AddStringToObject(item, "call_id", std::string{call_id}.c_str());
        cJSON_AddStringToObject(item, "output", std::string{output_json}.c_str());
        const bool item_ok = send_json(root);
        cJSON_Delete(root);
        if (!item_ok) {
            return tl::unexpected{ConversationError::SendFailed};
        }

        // Server VAD does not auto-continue after a tool result — ask for it.
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "response.create");
        const bool resp_ok = send_json(resp);
        cJSON_Delete(resp);
        if (!resp_ok) {
            return tl::unexpected{ConversationError::SendFailed};
        }
        pending_tool_call_ = false;
        return {};
    }

    tl::expected<void, ConversationError> cancel_response()
    {
        std::lock_guard lock{send_mutex_};
        if (client_ == nullptr || !esp_websocket_client_is_connected(client_)) {
            return tl::unexpected{ConversationError::NotConnected};
        }
        // Only send response.cancel when a response is actually still being
        // generated. The server streams replies faster than realtime, so by
        // the time the user barges in the response is usually already done —
        // sending response.cancel then just yields a "no active response"
        // server error.
        if (response_active_.exchange(false, std::memory_order_relaxed)) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "type", "response.cancel");
            const bool ok = send_json(root);
            cJSON_Delete(root);
            if (!ok) {
                return tl::unexpected{ConversationError::SendFailed};
            }
        }
        // The turn is over from our side — go straight back to Listening so
        // push_audio is accepted again immediately.
        pending_tool_call_ = false;
        set_state(ConversationState::Listening);
        return {};
    }

    void set_event_callback(EventCallback cb) { event_callback_ = std::move(cb); }

    ConversationState state() const { return state_.load(std::memory_order_relaxed); }

private:
    // ---- lifecycle ---------------------------------------------------------

    void teardown()
    {
        if (client_ != nullptr) {
            esp_websocket_client_stop(client_);
            esp_websocket_client_destroy(client_);
            client_ = nullptr;
        }
        if (rx_buffer_ != nullptr) {
            heap_caps_free(rx_buffer_);
            rx_buffer_ = nullptr;
        }
        rx_capacity_ = 0;
        rx_len_ = 0;
        set_state(ConversationState::Idle);
    }

    void set_state(ConversationState s)
    {
        const ConversationState prev = state_.exchange(s, std::memory_order_relaxed);
        if (prev != s) {
            ConversationEvent ev{};
            ev.type = ConversationEventType::StateChanged;
            ev.state = s;
            emit(ev);
        }
    }

    void emit(const ConversationEvent& ev)
    {
        if (event_callback_) {
            event_callback_(ev);
        }
    }

    void emit_error(ConversationError code, std::string message)
    {
        ConversationEvent ev{};
        ev.type = ConversationEventType::Error;
        ev.error = code;
        ev.text = std::move(message);
        emit(ev);
    }

    // ---- WebSocket transport ----------------------------------------------

    static void websocket_event_trampoline(void* arg, esp_event_base_t /*base*/, std::int32_t event_id, void* data)
    {
        static_cast<Impl*>(arg)->on_websocket_event(event_id, static_cast<esp_websocket_event_data_t*>(data));
    }

    void on_websocket_event(std::int32_t event_id, const esp_websocket_event_data_t* data)
    {
        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(kTag, "WEBSOCKET_EVENT_CONNECTED");
            send_session_update();
            break;
        case WEBSOCKET_EVENT_DATA:
            on_websocket_data(data);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(kTag, "WEBSOCKET_EVENT_DISCONNECTED");
            set_state(ConversationState::Error);
            emit_error(ConversationError::NotConnected, "websocket disconnected");
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(kTag, "WEBSOCKET_EVENT_ERROR");
            set_state(ConversationState::Error);
            emit_error(ConversationError::TransportInit, "websocket error");
            break;
        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(kTag, "WEBSOCKET_EVENT_CLOSED");
            set_state(ConversationState::Idle);
            break;
        default:
            break;
        }
    }

    void on_websocket_data(const esp_websocket_event_data_t* data)
    {
        if (data == nullptr || data->data_len < 0) {
            return;
        }
        // Only text frames carry JSON. op_code 0 is a continuation frame —
        // keep the op_code latched from the first fragment.
        if (data->payload_offset == 0) {
            rx_len_ = 0;
            rx_op_code_ = data->op_code;
        }
        if (rx_op_code_ != 0x01) {
            return; // not a text message (ping/pong/binary/close)
        }
        const std::size_t total = static_cast<std::size_t>(data->payload_len);
        if (total >= rx_capacity_) {
            ESP_LOGE(kTag, "RX frame too large (%u >= %u), dropped", static_cast<unsigned>(total),
                     static_cast<unsigned>(rx_capacity_));
            rx_len_ = 0;
            emit_error(ConversationError::ProtocolError, "rx frame too large");
            return;
        }
        const std::size_t chunk = static_cast<std::size_t>(data->data_len);
        if (rx_len_ + chunk >= rx_capacity_) {
            rx_len_ = 0;
            return;
        }
        std::memcpy(rx_buffer_ + rx_len_, data->data_ptr, chunk);
        rx_len_ += chunk;

        if (rx_len_ >= total && total > 0) {
            rx_buffer_[rx_len_] = '\0';
            parse_server_event(rx_buffer_, rx_len_);
            rx_len_ = 0;
        }
    }

    bool send_json(cJSON* root)
    {
        char* text = cJSON_PrintUnformatted(root);
        if (text == nullptr) {
            return false;
        }
        const int rc = esp_websocket_client_send_text(client_, text, std::strlen(text), kSendTimeout);
        cJSON_free(text);
        return rc >= 0;
    }

    // ---- outbound: session.update -----------------------------------------

    void send_session_update()
    {
        // GA Realtime API session.update payload:
        //   { "type": "session.update",
        //     "session": { "type": "realtime",
        //                  "model": "...",
        //                  "output_modalities": ["audio"],
        //                  "instructions": "...",
        //                  "audio": { "input":  { "format": {...}, "turn_detection": {...},
        //                                         "transcription": {...} },
        //                             "output": { "format": {...}, "voice": "..." } },
        //                  "tools": [...], "tool_choice": "auto" } }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "session.update");
        cJSON* session = cJSON_AddObjectToObject(root, "session");
        cJSON_AddStringToObject(session, "type", "realtime");

        if (!config_.model.empty()) {
            cJSON_AddStringToObject(session, "model", config_.model.c_str());
        }

        cJSON* mods = cJSON_AddArrayToObject(session, "output_modalities");
        cJSON_AddItemToArray(mods, cJSON_CreateString("audio"));

        if (!config_.instructions.empty()) {
            cJSON_AddStringToObject(session, "instructions", config_.instructions.c_str());
        }

        // ---- audio.input / audio.output ----
        cJSON* audio = cJSON_AddObjectToObject(session, "audio");

        cJSON* in = cJSON_AddObjectToObject(audio, "input");
        cJSON* in_fmt = cJSON_AddObjectToObject(in, "format");
        cJSON_AddStringToObject(in_fmt, "type", "audio/pcm");
        cJSON_AddNumberToObject(in_fmt, "rate", config_.input_sample_rate_hz);

        cJSON* vad = cJSON_AddObjectToObject(in, "turn_detection");
        cJSON_AddStringToObject(vad, "type", "server_vad");
        cJSON_AddNumberToObject(vad, "threshold", config_.vad_threshold);
        cJSON_AddNumberToObject(vad, "prefix_padding_ms", config_.vad_prefix_padding_ms);
        cJSON_AddNumberToObject(vad, "silence_duration_ms", config_.vad_silence_ms);

        if (config_.enable_input_transcription) {
            cJSON* tr = cJSON_AddObjectToObject(in, "transcription");
            cJSON_AddStringToObject(tr, "model", "whisper-1");
        }

        cJSON* out = cJSON_AddObjectToObject(audio, "output");
        cJSON* out_fmt = cJSON_AddObjectToObject(out, "format");
        if (output_is_ulaw_) {
            cJSON_AddStringToObject(out_fmt, "type", "audio/pcmu"); // G.711 µ-law @ 8 kHz
        } else {
            cJSON_AddStringToObject(out_fmt, "type", "audio/pcm");
            cJSON_AddNumberToObject(out_fmt, "rate", config_.output_sample_rate_hz);
        }
        if (!config_.voice.empty()) {
            cJSON_AddStringToObject(out, "voice", config_.voice.c_str());
        }

        // ---- tools ----
        cJSON* tools = cJSON_AddArrayToObject(session, "tools");
        for (const auto& tool : config_.tools) {
            cJSON* t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "type", "function");
            cJSON_AddStringToObject(t, "name", tool.name.c_str());
            cJSON_AddStringToObject(t, "description", tool.description.c_str());
            cJSON* params = cJSON_Parse(tool.parameters_json.c_str());
            if (params != nullptr) {
                cJSON_AddItemToObject(t, "parameters", params);
            }
            cJSON_AddItemToArray(tools, t);
        }
        cJSON_AddStringToObject(session, "tool_choice", "auto");

        {
            std::lock_guard lock{send_mutex_};
            send_json(root);
        }
        cJSON_Delete(root);
        ESP_LOGI(kTag, "session.update sent (%u tools)", static_cast<unsigned>(config_.tools.size()));
    }

    // ---- inbound: server event dispatch -----------------------------------

    static const char* json_str(const cJSON* obj, const char* key)
    {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
        return cJSON_IsString(item) ? item->valuestring : nullptr;
    }

    void parse_server_event(const char* json, std::size_t len)
    {
        cJSON* root = cJSON_ParseWithLength(json, len);
        if (root == nullptr) {
            ESP_LOGW(kTag, "failed to parse server event");
            return;
        }
        const char* type = json_str(root, "type");
        if (type == nullptr) {
            cJSON_Delete(root);
            return;
        }

        if (std::strcmp(type, "session.created") == 0 || std::strcmp(type, "session.updated") == 0) {
            ESP_LOGI(kTag, "%s", type);
            if (!session_updated_) {
                session_updated_ = true;
                set_state(ConversationState::Listening);
            }
        } else if (std::strcmp(type, "input_audio_buffer.speech_started") == 0) {
            emit_simple(ConversationEventType::SpeechStarted);
            set_state(ConversationState::Listening);
        } else if (std::strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
            emit_simple(ConversationEventType::SpeechStopped);
            set_state(ConversationState::Thinking);
        } else if (std::strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
            const char* transcript = json_str(root, "transcript");
            ConversationEvent ev{};
            ev.type = ConversationEventType::UserTranscript;
            ev.text = transcript != nullptr ? transcript : "";
            emit(ev);
        } else if (std::strcmp(type, "response.created") == 0) {
            response_active_.store(true, std::memory_order_relaxed);
        } else if (std::strcmp(type, "response.output_audio.delta") == 0 ||
                   std::strcmp(type, "response.audio.delta") == 0) { // GA / beta names
            handle_audio_delta(root);
        } else if (std::strcmp(type, "response.output_audio.done") == 0 ||
                   std::strcmp(type, "response.audio.done") == 0) {
            emit_simple(ConversationEventType::AssistantAudioDone);
        } else if (std::strcmp(type, "response.output_audio_transcript.delta") == 0 ||
                   std::strcmp(type, "response.audio_transcript.delta") == 0 ||
                   std::strcmp(type, "response.text.delta") == 0) {
            const char* delta = json_str(root, "delta");
            ConversationEvent ev{};
            ev.type = ConversationEventType::AssistantTextDelta;
            ev.text = delta != nullptr ? delta : "";
            emit(ev);
        } else if (std::strcmp(type, "response.output_audio_transcript.done") == 0 ||
                   std::strcmp(type, "response.audio_transcript.done") == 0 ||
                   std::strcmp(type, "response.text.done") == 0) {
            const char* full = json_str(root, "transcript");
            if (full == nullptr) {
                full = json_str(root, "text");
            }
            ConversationEvent ev{};
            ev.type = ConversationEventType::AssistantTextDone;
            ev.text = full != nullptr ? full : "";
            emit(ev);
        } else if (std::strcmp(type, "response.function_call_arguments.delta") == 0) {
            const char* call_id = json_str(root, "call_id");
            const char* delta = json_str(root, "delta");
            if (call_id != nullptr && delta != nullptr) {
                fn_args_[call_id] += delta;
            }
        } else if (std::strcmp(type, "response.function_call_arguments.done") == 0) {
            handle_function_call_done(root);
        } else if (std::strcmp(type, "response.done") == 0) {
            response_active_.store(false, std::memory_order_relaxed);
            emit_simple(ConversationEventType::ResponseDone);
            if (!pending_tool_call_) {
                set_state(ConversationState::Listening);
            }
        } else if (std::strcmp(type, "error") == 0) {
            const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
            const char* msg = err != nullptr ? json_str(err, "message") : nullptr;
            ESP_LOGE(kTag, "server error: %s", msg != nullptr ? msg : "(unknown)");
            emit_error(ConversationError::ServerError, msg != nullptr ? msg : "server error");
        } else {
            ESP_LOGD(kTag, "unhandled event: %s", type);
        }

        cJSON_Delete(root);
    }

    void emit_simple(ConversationEventType type)
    {
        ConversationEvent ev{};
        ev.type = type;
        emit(ev);
    }

    void handle_audio_delta(const cJSON* root)
    {
        const char* delta = json_str(root, "delta");
        if (delta == nullptr) {
            return;
        }
        auto decoded = base64::decode(delta);
        if (!decoded) {
            emit_error(decoded.error(), "audio base64 decode failed");
            return;
        }
        const auto& bytes = *decoded;
        std::shared_ptr<std::vector<std::int16_t>> pcm;
        if (output_is_ulaw_) {
            // G.711 µ-law: one byte per 8 kHz sample, expand to PCM16.
            pcm = std::make_shared<std::vector<std::int16_t>>(bytes.size());
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                (*pcm)[i] = ulaw_to_pcm16(bytes[i]);
            }
        } else {
            // pcm16: raw little-endian 16-bit samples.
            pcm = std::make_shared<std::vector<std::int16_t>>(bytes.size() / sizeof(std::int16_t));
            std::memcpy(pcm->data(), bytes.data(), pcm->size() * sizeof(std::int16_t));
        }

        if (state_.load(std::memory_order_relaxed) != ConversationState::Speaking) {
            set_state(ConversationState::Speaking);
        }
        ConversationEvent ev{};
        ev.type = ConversationEventType::AssistantAudioChunk;
        ev.audio = std::move(pcm);
        emit(ev);
    }

    void handle_function_call_done(const cJSON* root)
    {
        const char* call_id = json_str(root, "call_id");
        const char* name = json_str(root, "name");
        const char* arguments = json_str(root, "arguments");

        ToolCall call{};
        call.call_id = call_id != nullptr ? call_id : "";
        call.name = name != nullptr ? name : "";
        if (arguments != nullptr) {
            call.arguments_json = arguments;
        } else if (call_id != nullptr) {
            auto it = fn_args_.find(call_id);
            if (it != fn_args_.end()) {
                call.arguments_json = it->second;
            }
        }
        if (call_id != nullptr) {
            fn_args_.erase(call_id);
        }
        pending_tool_call_ = true;

        ConversationEvent ev{};
        ev.type = ConversationEventType::ToolCallRequested;
        ev.tool_call = std::move(call);
        emit(ev);
    }

    // ---- members -----------------------------------------------------------

    std::string api_key_;
    ConversationConfig config_{};
    EventCallback event_callback_{};

    esp_websocket_client_handle_t client_{nullptr};
    std::atomic<ConversationState> state_{ConversationState::Idle};
    std::mutex send_mutex_;

    bool session_updated_{false};
    bool pending_tool_call_{false};
    bool output_is_ulaw_{false};            // wire codec for assistant audio (g711_ulaw vs pcm16)
    std::atomic<bool> response_active_{false}; // a response is mid-generation (set/cleared from the WS task)

    // RX frame reassembly (PSRAM).
    char* rx_buffer_{nullptr};
    std::size_t rx_capacity_{0};
    std::size_t rx_len_{0};
    std::uint8_t rx_op_code_{0};

    // Hot-path scratch for input_audio_buffer.append.
    std::vector<char> b64_scratch_;
    std::vector<char> json_scratch_;

    // Function-call argument accumulation, keyed by call_id.
    std::unordered_map<std::string, std::string> fn_args_;
};

OpenAiRealtimeClient::OpenAiRealtimeClient(std::string api_key)
    : impl_{std::make_unique<Impl>(std::move(api_key))}
{
}

OpenAiRealtimeClient::~OpenAiRealtimeClient() = default;

void OpenAiRealtimeClient::set_event_callback(EventCallback cb)
{
    impl_->set_event_callback(std::move(cb));
}

tl::expected<void, ConversationError> OpenAiRealtimeClient::start(const ConversationConfig& config)
{
    return impl_->start(config);
}

void OpenAiRealtimeClient::stop()
{
    impl_->stop();
}

tl::expected<void, ConversationError> OpenAiRealtimeClient::push_audio(std::span<const std::int16_t> pcm)
{
    return impl_->push_audio(pcm);
}

tl::expected<void, ConversationError> OpenAiRealtimeClient::commit_audio()
{
    // OpenAI Realtime uses server-side VAD — end-of-utterance is detected
    // remotely, so there is nothing to commit explicitly.
    return {};
}

tl::expected<void, ConversationError> OpenAiRealtimeClient::submit_tool_result(std::string_view call_id,
                                                                               std::string_view output_json)
{
    return impl_->submit_tool_result(call_id, output_json);
}

tl::expected<void, ConversationError> OpenAiRealtimeClient::cancel_response()
{
    return impl_->cancel_response();
}

ConversationState OpenAiRealtimeClient::state() const
{
    return impl_->state();
}

} // namespace stackchan::conversation
