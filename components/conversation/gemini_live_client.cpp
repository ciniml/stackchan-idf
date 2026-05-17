// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "conversation/gemini_live_client.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>

#include "base64.hpp"
#include "psram_allocator.hpp"

namespace stackchan::conversation {

namespace {

constexpr const char* kTag = "gemini-live";

// Endpoint. The API key rides in the URL query string — Gemini Live does
// not accept an Authorization header. Model is set via the setup message,
// not the URL, so the host string is constant.
constexpr const char* kHost = "generativelanguage.googleapis.com";
constexpr const char* kPath = "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";

// Per-send timeout for the sender task's WS write. The mic loop never blocks
// on this (push_audio() enqueues), so a long write here just delays the
// sender. esp_websocket_client aborts the connection on a 0-byte write
// when poll_write times out, so this is effectively our "give up and let
// recovery reconnect" threshold.
constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(10000);

constexpr UBaseType_t kSenderTaskPrio = 5;
constexpr std::size_t kSenderTaskStack = 6144;
constexpr BaseType_t kSenderTaskCore = 1;

// Audio tx ring. PCM16 @ 16 kHz mono = 32 KB/s; 40 ms chunks = 640 samples
// (1280 bytes) each. 96 slots ≈ 3.8 s buffer for hiccups before evictions.
// Worst-case ~120 KiB sitting in PSRAM.
constexpr std::size_t kAudioTxQueueLen = 96;

// Hot-path scratch ceiling for the sender's base64 / JSON wrap. 2048 PCM16
// samples × 2 B = 4096 B raw; base64 + JSON envelope ~5800 B at peak.
constexpr std::size_t kMaxChunkSamples = 2048;

struct AudioChunk {
    std::uint16_t len_samples;  // PCM16 little-endian samples
    std::int16_t data[];        // [len_samples]
};

} // namespace

class GeminiLiveClient::Impl {
public:
    explicit Impl(std::string api_key) : api_key_{std::move(api_key)} {}

    ~Impl() { teardown(); }

    tl::expected<void, ConversationError> start(const ConversationConfig& config)
    {
        if (client_ != nullptr) {
            return tl::unexpected{ConversationError::InvalidState};
        }
        config_ = config;

        // The URL includes the API key as ?key=... — esp_websocket_client
        // hands it through to the transport unchanged.
        const std::string uri = std::string{"wss://"} + kHost + kPath + "?key=" + api_key_;

        esp_websocket_client_config_t cfg{};
        cfg.uri = uri.c_str();
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.buffer_size = 8192;
        cfg.task_stack = 6144;
        cfg.task_prio = 5;
        cfg.disable_auto_reconnect = true;
        cfg.network_timeout_ms = 10000;
        cfg.ping_interval_sec = 15;
        cfg.pingpong_timeout_sec = 25;
        cfg.keep_alive_enable = true;
        cfg.keep_alive_idle = 10;
        cfg.keep_alive_interval = 5;
        cfg.keep_alive_count = 3;

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
        json_scratch_.assign(b64_cap + 256, '\0');

        client_ = esp_websocket_client_init(&cfg);
        if (client_ == nullptr) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }
        esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY,
                                       &Impl::websocket_event_trampoline, this);

        setup_sent_ = false;
        audio_seq_ = 0;
        set_state(ConversationState::Connecting);

        audio_tx_queue_ = xQueueCreate(kAudioTxQueueLen, sizeof(AudioChunk*));
        if (audio_tx_queue_ == nullptr) {
            teardown();
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        sender_should_exit_.store(false, std::memory_order_relaxed);
        if (xTaskCreatePinnedToCoreWithCaps(&Impl::sender_trampoline, "gemini_tx",
                                            kSenderTaskStack, this, kSenderTaskPrio,
                                            &sender_task_, kSenderTaskCore,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }

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
            return {};
        }
        if (pcm.empty()) {
            return {};
        }
        if (audio_tx_queue_ == nullptr) {
            return tl::unexpected{ConversationError::NotConnected};
        }
        if (pcm.size() > kMaxChunkSamples) {
            return tl::unexpected{ConversationError::InvalidState};
        }

        // Allocate the chunk in PSRAM, copy raw PCM16 samples (LE on this
        // chip already — no conversion needed; Gemini wants exactly that).
        const std::size_t alloc =
            sizeof(AudioChunk) + pcm.size() * sizeof(std::int16_t);
        auto* chunk = static_cast<AudioChunk*>(
            heap_caps_malloc(alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (chunk == nullptr) {
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        chunk->len_samples = static_cast<std::uint16_t>(pcm.size());
        std::memcpy(chunk->data, pcm.data(), pcm.size() * sizeof(std::int16_t));

        if (xQueueSend(audio_tx_queue_, &chunk, 0) != pdTRUE) {
            AudioChunk* old = nullptr;
            if (xQueueReceive(audio_tx_queue_, &old, 0) == pdTRUE && old != nullptr) {
                heap_caps_free(old);
            }
            if (xQueueSend(audio_tx_queue_, &chunk, 0) != pdTRUE) {
                heap_caps_free(chunk);
                ESP_LOGW(kTag, "audio tx queue stuck; dropping chunk");
                return tl::unexpected{ConversationError::SendFailed};
            }
            ESP_LOGW(kTag, "audio tx queue full; evicted oldest chunk");
        }
        return {};
    }

    tl::expected<void, ConversationError> commit_audio()
    {
        // Server VAD detects turn boundaries automatically.
        return {};
    }

    tl::expected<void, ConversationError> submit_tool_result(std::string_view call_id,
                                                              std::string_view output_json)
    {
        std::lock_guard lock{send_mutex_};
        if (client_ == nullptr || !esp_websocket_client_is_connected(client_)) {
            return tl::unexpected{ConversationError::NotConnected};
        }

        // Gemini expects:
        //   { "toolResponse":
        //     { "functionResponses":
        //       [ { "id": <id>, "name": <name>, "response": <object> } ] } }
        // We don't have the function name here — the caller usually returns
        // the result with the call_id alone, so leave name empty and let the
        // server look up by id. Parse output_json into a cJSON object; if it
        // isn't valid JSON, wrap it as {"output": "<string>"} so the server
        // still gets something coherent.
        cJSON* root = cJSON_CreateObject();
        cJSON* tr = cJSON_AddObjectToObject(root, "toolResponse");
        cJSON* arr = cJSON_AddArrayToObject(tr, "functionResponses");
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", std::string{call_id}.c_str());

        cJSON* parsed = cJSON_Parse(std::string{output_json}.c_str());
        if (parsed != nullptr && cJSON_IsObject(parsed)) {
            cJSON_AddItemToObject(item, "response", parsed);
        } else {
            if (parsed != nullptr) cJSON_Delete(parsed);
            cJSON* resp = cJSON_AddObjectToObject(item, "response");
            cJSON_AddStringToObject(resp, "output", std::string{output_json}.c_str());
        }
        cJSON_AddItemToArray(arr, item);

        const bool ok = send_json(root);
        cJSON_Delete(root);
        return ok ? tl::expected<void, ConversationError>{}
                  : tl::unexpected{ConversationError::SendFailed};
    }

    tl::expected<void, ConversationError> cancel_response()
    {
        // Gemini Live triggers barge-in server-side from incoming audio
        // (interrupted event). No client-initiated cancel exists in the
        // protocol; the local conv-task already stops draining the audio
        // queue on barge-in, so this is a documented no-op here.
        return {};
    }

    ConversationState state() const { return state_.load(std::memory_order_relaxed); }

    void set_event_callback(EventCallback cb) { event_callback_ = std::move(cb); }

private:
    // ---- lifecycle ---------------------------------------------------------

    void teardown()
    {
        if (sender_task_ != nullptr) {
            sender_should_exit_.store(true, std::memory_order_release);
            AudioChunk* sentinel = nullptr;
            xQueueSend(audio_tx_queue_, &sentinel, 0);
            for (int i = 0; i < 200; ++i) {
                if (eTaskGetState(sender_task_) == eDeleted) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            sender_task_ = nullptr;
        }
        if (audio_tx_queue_ != nullptr) {
            AudioChunk* chunk = nullptr;
            while (xQueueReceive(audio_tx_queue_, &chunk, 0) == pdTRUE) {
                if (chunk != nullptr) heap_caps_free(chunk);
            }
            vQueueDelete(audio_tx_queue_);
            audio_tx_queue_ = nullptr;
        }

        if (client_ != nullptr) {
            esp_websocket_client_stop(client_);
            esp_websocket_client_destroy(client_);
            client_ = nullptr;
        }
        if (rx_buffer_ != nullptr) {
            heap_caps_free(rx_buffer_);
            rx_buffer_ = nullptr;
        }
        b64_scratch_.clear();
        b64_scratch_.shrink_to_fit();
        json_scratch_.clear();
        json_scratch_.shrink_to_fit();
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
        if (event_callback_) event_callback_(ev);
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

    static void websocket_event_trampoline(void* arg, esp_event_base_t /*base*/,
                                            std::int32_t event_id, void* data)
    {
        static_cast<Impl*>(arg)->on_websocket_event(
            event_id, static_cast<esp_websocket_event_data_t*>(data));
    }

    void on_websocket_event(std::int32_t event_id, const esp_websocket_event_data_t* data)
    {
        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(kTag, "WEBSOCKET_EVENT_CONNECTED");
            send_setup();
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
        if (data == nullptr || data->data_len < 0) return;
        // Gemini ships JSON over text frames; binary frames are unused.
        if (data->payload_offset == 0) {
            rx_len_ = 0;
            rx_op_code_ = data->op_code;
        }
        if (rx_op_code_ != 0x01) return; // not text
        const std::size_t total = static_cast<std::size_t>(data->payload_len);
        const std::size_t chunk = static_cast<std::size_t>(data->data_len);
        if (rx_len_ + chunk > rx_capacity_) {
            ESP_LOGW(kTag, "rx overflow: total=%u capacity=%u",
                     static_cast<unsigned>(total), static_cast<unsigned>(rx_capacity_));
            rx_len_ = 0;
            return;
        }
        std::memcpy(rx_buffer_ + rx_len_, data->data_ptr, chunk);
        rx_len_ += chunk;
        if (rx_len_ < total) return; // wait for the rest
        parse_server_event(rx_buffer_, rx_len_);
        rx_len_ = 0;
    }

    // ---- outbound ----------------------------------------------------------

    bool send_json(cJSON* root)
    {
        // send_mutex_ must be held by the caller.
        char* str = cJSON_PrintUnformatted(root);
        if (str == nullptr) return false;
        const int rc = esp_websocket_client_send_text(client_, str, std::strlen(str), kSendTimeout);
        cJSON_free(str);
        return rc > 0;
    }

    void send_setup()
    {
        // BidiGenerateContentSetup. Tools, system prompt, audio modality,
        // optional session resumption handle from a prior connection.
        cJSON* root = cJSON_CreateObject();
        cJSON* setup = cJSON_AddObjectToObject(root, "setup");

        // Default model if caller didn't override.
        const std::string model = config_.model.empty()
            ? std::string{"models/gemini-2.0-flash-live-001"}
            : (config_.model.starts_with("models/") ? config_.model
                                                    : std::string{"models/"} + config_.model);
        cJSON_AddStringToObject(setup, "model", model.c_str());

        cJSON* gen_cfg = cJSON_AddObjectToObject(setup, "generationConfig");
        cJSON* mods = cJSON_AddArrayToObject(gen_cfg, "responseModalities");
        cJSON_AddItemToArray(mods, cJSON_CreateString("AUDIO"));
        if (!config_.voice.empty()) {
            cJSON* speech_cfg = cJSON_AddObjectToObject(gen_cfg, "speechConfig");
            cJSON* voice_cfg = cJSON_AddObjectToObject(speech_cfg, "voiceConfig");
            cJSON* prebuilt = cJSON_AddObjectToObject(voice_cfg, "prebuiltVoiceConfig");
            cJSON_AddStringToObject(prebuilt, "voiceName", config_.voice.c_str());
        }

        if (!config_.instructions.empty()) {
            cJSON* sys = cJSON_AddObjectToObject(setup, "systemInstruction");
            cJSON* parts = cJSON_AddArrayToObject(sys, "parts");
            cJSON* part = cJSON_CreateObject();
            cJSON_AddStringToObject(part, "text", config_.instructions.c_str());
            cJSON_AddItemToArray(parts, part);
        }

        if (config_.enable_input_transcription) {
            cJSON_AddObjectToObject(setup, "inputAudioTranscription");
            cJSON_AddObjectToObject(setup, "outputAudioTranscription");
        }

        if (!config_.tools.empty()) {
            cJSON* tools = cJSON_AddArrayToObject(setup, "tools");
            cJSON* tool = cJSON_CreateObject();
            cJSON* decls = cJSON_AddArrayToObject(tool, "functionDeclarations");
            for (const auto& t : config_.tools) {
                cJSON* d = cJSON_CreateObject();
                cJSON_AddStringToObject(d, "name", t.name.c_str());
                cJSON_AddStringToObject(d, "description", t.description.c_str());
                cJSON* params = cJSON_Parse(t.parameters_json.c_str());
                if (params != nullptr) {
                    cJSON_AddItemToObject(d, "parameters", params);
                }
                cJSON_AddItemToArray(decls, d);
            }
            cJSON_AddItemToArray(tools, tool);
        }

        // Session resumption: if we have a handle from a prior goAway, ask
        // the server to continue from there. The server will reply with new
        // handles via sessionResumptionUpdate which we cache for next time.
        // Always include the field so the server emits resumption updates;
        // a null/empty handle just means "start a fresh session, but please
        // give me resumption tokens".
        cJSON* rs = cJSON_AddObjectToObject(setup, "sessionResumption");
        if (!session_handle_.empty()) {
            cJSON_AddStringToObject(rs, "handle", session_handle_.c_str());
            ESP_LOGI(kTag, "resuming session via cached handle");
        }

        {
            std::lock_guard lock{send_mutex_};
            (void)send_json(root);
        }
        cJSON_Delete(root);
        ESP_LOGI(kTag, "setup sent (%u tools)", static_cast<unsigned>(config_.tools.size()));
        setup_sent_ = true;
    }

    // ---- inbound: server event dispatch -----------------------------------

    static const char* json_str(const cJSON* obj, const char* key)
    {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
        return cJSON_IsString(v) ? v->valuestring : nullptr;
    }

    void parse_server_event(const char* json, std::size_t len)
    {
        cJSON* root = cJSON_ParseWithLength(json, len);
        if (root == nullptr) {
            ESP_LOGW(kTag, "json parse failed");
            return;
        }

        // setupComplete: empty object, marks session ready.
        if (cJSON_GetObjectItemCaseSensitive(root, "setupComplete") != nullptr) {
            ESP_LOGI(kTag, "setupComplete");
            set_state(ConversationState::Listening);
        }

        if (cJSON* sc = cJSON_GetObjectItemCaseSensitive(root, "serverContent"); sc != nullptr) {
            handle_server_content(sc);
        }
        if (cJSON* tc = cJSON_GetObjectItemCaseSensitive(root, "toolCall"); tc != nullptr) {
            handle_tool_call(tc);
        }
        if (cJSON* up = cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate");
            up != nullptr) {
            handle_session_resumption_update(up);
        }
        if (cJSON* gw = cJSON_GetObjectItemCaseSensitive(root, "goAway"); gw != nullptr) {
            const cJSON* tb = cJSON_GetObjectItemCaseSensitive(gw, "timeBeforeClose");
            ESP_LOGW(kTag, "goAway received: timeBeforeClose=%s",
                     cJSON_IsString(tb) ? tb->valuestring : "?");
            // The server will close shortly; conv-task's existing recovery
            // path reconnects, and send_setup() replays session_handle_.
        }
        cJSON_Delete(root);
    }

    void handle_server_content(const cJSON* sc)
    {
        // inputTranscription / outputTranscription: text only.
        if (const cJSON* it = cJSON_GetObjectItemCaseSensitive(sc, "inputTranscription");
            it != nullptr) {
            const char* text = json_str(it, "text");
            if (text != nullptr) {
                ConversationEvent ev{};
                ev.type = ConversationEventType::UserTranscript;
                ev.text = text;
                emit(ev);
            }
        }
        if (const cJSON* ot = cJSON_GetObjectItemCaseSensitive(sc, "outputTranscription");
            ot != nullptr) {
            const char* text = json_str(ot, "text");
            if (text != nullptr) {
                ConversationEvent ev{};
                ev.type = ConversationEventType::AssistantTextDelta;
                ev.text = text;
                emit(ev);
            }
        }

        // modelTurn.parts[].inlineData = base64 PCM 24 kHz audio.
        if (const cJSON* mt = cJSON_GetObjectItemCaseSensitive(sc, "modelTurn"); mt != nullptr) {
            const cJSON* parts = cJSON_GetObjectItemCaseSensitive(mt, "parts");
            if (cJSON_IsArray(parts)) {
                const cJSON* part = nullptr;
                cJSON_ArrayForEach(part, parts) {
                    const cJSON* inline_data = cJSON_GetObjectItemCaseSensitive(part, "inlineData");
                    if (inline_data == nullptr) continue;
                    const char* mime = json_str(inline_data, "mimeType");
                    const char* b64 = json_str(inline_data, "data");
                    if (b64 == nullptr) continue;
                    auto decoded = base64::decode(b64);
                    if (!decoded) {
                        emit_error(decoded.error(), "audio base64 decode failed");
                        continue;
                    }
                    auto pcm = std::make_shared<std::vector<std::int16_t>>(
                        decoded->size() / sizeof(std::int16_t));
                    std::memcpy(pcm->data(), decoded->data(),
                                pcm->size() * sizeof(std::int16_t));
                    if (state_.load(std::memory_order_relaxed) != ConversationState::Speaking) {
                        set_state(ConversationState::Speaking);
                    }
                    ConversationEvent ev{};
                    ev.type = ConversationEventType::AssistantAudioChunk;
                    ev.audio = std::move(pcm);
                    emit(ev);
                    (void)mime; // currently we trust the server's "audio/pcm;rate=24000"
                }
            }
        }

        // interrupted: barge-in (user spoke over the model).
        const cJSON* interrupted = cJSON_GetObjectItemCaseSensitive(sc, "interrupted");
        if (cJSON_IsTrue(interrupted)) {
            ESP_LOGI(kTag, "serverContent.interrupted");
        }

        // generationComplete: model has finished generating audio. Mirror
        // OpenAI's AssistantAudioDone so the conv-task knows playback can
        // finish naturally.
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "generationComplete"))) {
            ConversationEvent ev{};
            ev.type = ConversationEventType::AssistantAudioDone;
            emit(ev);
        }
        // turnComplete: server ready for the next user turn.
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "turnComplete"))) {
            ConversationEvent ev{};
            ev.type = ConversationEventType::ResponseDone;
            emit(ev);
        }
    }

    void handle_tool_call(const cJSON* tc)
    {
        const cJSON* calls = cJSON_GetObjectItemCaseSensitive(tc, "functionCalls");
        if (!cJSON_IsArray(calls)) return;
        const cJSON* call = nullptr;
        cJSON_ArrayForEach(call, calls) {
            const char* id = json_str(call, "id");
            const char* name = json_str(call, "name");
            const cJSON* args = cJSON_GetObjectItemCaseSensitive(call, "args");
            if (id == nullptr || name == nullptr) continue;
            char* args_str = (args != nullptr) ? cJSON_PrintUnformatted(args) : nullptr;
            ConversationEvent ev{};
            ev.type = ConversationEventType::ToolCallRequested;
            ev.tool_call = ToolCall{
                .call_id = id,
                .name = name,
                .arguments_json = args_str != nullptr ? std::string{args_str} : std::string{"{}"},
            };
            emit(ev);
            if (args_str != nullptr) cJSON_free(args_str);
        }
    }

    void handle_session_resumption_update(const cJSON* up)
    {
        // Gemini emits the new handle on every turn; the latest one wins.
        // We don't persist across reboots — the cached handle survives only
        // for in-process reconnects (e.g., goAway 15 min cap).
        const char* handle = json_str(up, "newHandle");
        const cJSON* resumable = cJSON_GetObjectItemCaseSensitive(up, "resumable");
        if (handle != nullptr && cJSON_IsTrue(resumable)) {
            session_handle_ = handle;
            ESP_LOGD(kTag, "cached new resumption handle (%u chars)",
                     static_cast<unsigned>(session_handle_.size()));
        }
    }

    // ---- audio sender task -------------------------------------------------

    static void sender_trampoline(void* arg)
    {
        static_cast<Impl*>(arg)->sender_loop();
        vTaskDeleteWithCaps(nullptr);
    }

    void sender_loop()
    {
        AudioChunk* chunk = nullptr;
        while (!sender_should_exit_.load(std::memory_order_acquire)) {
            if (xQueueReceive(audio_tx_queue_, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
                continue;
            }
            if (chunk == nullptr) break;
            send_one_chunk(chunk);
            heap_caps_free(chunk);
            chunk = nullptr;
        }
    }

    void send_one_chunk(AudioChunk* chunk)
    {
        if (client_ == nullptr) return;
        if (!esp_websocket_client_is_connected(client_)) return;
        if (!setup_sent_) return; // wait until setup has been emitted

        const auto* raw_ptr = reinterpret_cast<const std::uint8_t*>(chunk->data);
        const std::size_t raw_len = chunk->len_samples * sizeof(std::int16_t);
        const std::size_t needed_b64 = base64::encoded_size(raw_len);
        if (needed_b64 > b64_scratch_.size()) return;

        auto enc = base64::encode_into({raw_ptr, raw_len}, b64_scratch_);
        if (!enc) return;
        const int n = std::snprintf(
            json_scratch_.data(), json_scratch_.size(),
            "{\"realtimeInput\":{\"audio\":{\"data\":\"%.*s\",\"mimeType\":\"audio/pcm;rate=16000\"}}}",
            static_cast<int>(*enc), b64_scratch_.data());
        if (n <= 0 || static_cast<std::size_t>(n) >= json_scratch_.size()) return;

        const std::uint32_t t0 = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        int send_rc;
        {
            std::lock_guard lock{send_mutex_};
            send_rc = esp_websocket_client_send_text(client_, json_scratch_.data(), n, kSendTimeout);
        }
        const std::uint32_t dt = static_cast<std::uint32_t>(esp_timer_get_time() / 1000) - t0;
        ++audio_seq_;
        if (send_rc <= 0) {
            ESP_LOGW(kTag, "audio send failed seq=%lu dt=%lums size=%dB rc=%d",
                     static_cast<unsigned long>(audio_seq_),
                     static_cast<unsigned long>(dt), n, send_rc);
        } else if (dt >= 100 || (audio_seq_ % 100) == 1) {
            const UBaseType_t queued = uxQueueMessagesWaiting(audio_tx_queue_);
            ESP_LOGI(kTag, "audio send seq=%lu dt=%lums queued=%u",
                     static_cast<unsigned long>(audio_seq_),
                     static_cast<unsigned long>(dt),
                     static_cast<unsigned>(queued));
        }
    }

    // ---- members -----------------------------------------------------------

    std::string api_key_;
    ConversationConfig config_;
    EventCallback event_callback_;

    esp_websocket_client_handle_t client_{nullptr};
    std::atomic<ConversationState> state_{ConversationState::Idle};
    std::mutex send_mutex_;

    bool setup_sent_{false};
    std::uint32_t audio_seq_{0};

    // Resumption handle from goAway / sessionResumptionUpdate. Empty means
    // no prior session.
    std::string session_handle_;

    char* rx_buffer_{nullptr};
    std::size_t rx_capacity_{0};
    std::size_t rx_len_{0};
    std::uint8_t rx_op_code_{0};

    std::vector<char, PsramAllocator<char>> b64_scratch_;
    std::vector<char, PsramAllocator<char>> json_scratch_;

    QueueHandle_t audio_tx_queue_{nullptr};
    TaskHandle_t sender_task_{nullptr};
    std::atomic<bool> sender_should_exit_{false};
};

// ---- public class plumbing ------------------------------------------------

GeminiLiveClient::GeminiLiveClient(std::string api_key)
    : impl_{std::make_unique<Impl>(std::move(api_key))}
{
}

GeminiLiveClient::~GeminiLiveClient() = default;

void GeminiLiveClient::set_event_callback(EventCallback cb)
{
    impl_->set_event_callback(std::move(cb));
}

tl::expected<void, ConversationError>
GeminiLiveClient::start(const ConversationConfig& config)
{
    return impl_->start(config);
}

void GeminiLiveClient::stop() { impl_->stop(); }

tl::expected<void, ConversationError>
GeminiLiveClient::push_audio(std::span<const std::int16_t> pcm)
{
    return impl_->push_audio(pcm);
}

tl::expected<void, ConversationError> GeminiLiveClient::commit_audio()
{
    return impl_->commit_audio();
}

tl::expected<void, ConversationError>
GeminiLiveClient::submit_tool_result(std::string_view call_id, std::string_view output_json)
{
    return impl_->submit_tool_result(call_id, output_json);
}

tl::expected<void, ConversationError> GeminiLiveClient::cancel_response()
{
    return impl_->cancel_response();
}

ConversationState GeminiLiveClient::state() const { return impl_->state(); }

} // namespace stackchan::conversation
