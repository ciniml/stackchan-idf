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
#include <esp_timer.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>

#include "base64.hpp"
#include "psram_allocator.hpp"
#include "ws_extra_headers.hpp"

namespace stackchan::conversation {

namespace {

constexpr const char* kTag = "openai-rt";

// wss host + path. The model is appended as a query parameter.
constexpr const char* kRealtimeUriPrefix = "wss://api.openai.com/v1/realtime?model=";

// Hot-path scratch sized for this many wire bytes per push_audio chunk.
// At 8 kHz µ-law a 40 ms chunk is 320 bytes; 1024 leaves headroom for any
// future change without re-sizing on the hot path.
constexpr std::size_t kMaxChunkBytes = 1024;

// Per-send timeout for the WS write inside the sender task. The mic loop no
// longer blocks on this — it enqueues into the audio tx ring and returns
// immediately, so a long write here just delays the sender, not capture.
// esp_websocket_client unconditionally aborts the connection on a send that
// returns 0 bytes (its `poll_write` timed out), so this is effectively our
// "give up and reconnect" threshold. Generous enough that a brief BLE-coex
// or Wi-Fi DTIM stall doesn't tear the session down.
constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(10000);

// Background sender task: drains audio_tx_queue_, encodes each chunk to
// base64+JSON, calls esp_websocket_client_send_text. Pinned to core 1 so it
// doesn't fight with the conv-task / render-task pair on core 0.
constexpr UBaseType_t kSenderTaskPrio = 5;
constexpr std::size_t kSenderTaskStack = 6144;
constexpr BaseType_t kSenderTaskCore = 1;

// Capacity (chunks) of the audio tx ring buffer between push_audio() and the
// sender. At 40 ms mic chunks (= 320 µ-law bytes each at 8 kHz) this is 96
// slots * 40 ms = ~3.8 s of audio. Lets the sender pause for that long
// during a network hiccup before the oldest chunks start being evicted.
// Each chunk is ~320 B μ-law allocated in PSRAM, so worst-case ~31 KiB.
constexpr std::size_t kAudioTxQueueLen = 96;

// Per-chunk wire payload (µ-law bytes), allocated in PSRAM. The trailing
// flexible array avoids a second heap allocation per chunk.
struct AudioChunk {
    std::uint16_t len_bytes;
    std::uint8_t data[];  // [len_bytes] G.711 µ-law samples
};

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

// Standard G.711 µ-law encode (Sun algorithm). 16-bit PCM in, one companded
// byte out. Used to shrink the mic uplink from PCM16 @ 24 kHz (~65 KB/s
// JSON+base64) down to PCM_MU @ 8 kHz (~11 KB/s) so coex hiccups don't
// build up enough backlog to evict the audio tx ring.
std::uint8_t pcm16_to_ulaw(std::int16_t pcm)
{
    constexpr int kBias = 0x84;
    constexpr int kClip = 32635;
    std::uint8_t sign = (pcm < 0) ? 0x80 : 0x00;
    int mag = sign ? -static_cast<int>(pcm) : static_cast<int>(pcm);
    if (mag > kClip) mag = kClip;
    mag += kBias;
    int seg = 7;
    int test = 0x4000;
    while ((mag & test) == 0 && seg > 0) {
        test >>= 1;
        --seg;
    }
    const int mantissa = (mag >> (seg + 3)) & 0x0F;
    return static_cast<std::uint8_t>(~(sign | (seg << 4) | mantissa));
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
        tx_evicted_total_.store(0, std::memory_order_relaxed);
        last_evict_log_ms_ = 0;

        const std::size_t rx_cap = static_cast<std::size_t>(CONFIG_STACKCHAN_CONV_WS_RX_BUFFER);
        rx_buffer_ = static_cast<char*>(heap_caps_malloc(rx_cap, MALLOC_CAP_SPIRAM));
        if (rx_buffer_ == nullptr) {
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        rx_capacity_ = rx_cap;
        rx_len_ = 0;
        rx_op_code_ = 0;

        const std::size_t b64_cap = base64::encoded_size(kMaxChunkBytes);
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
        apply_extra_ws_headers(client_, config_.extra_headers);
        // OpenAI Realtime is GA — the OpenAI-Beta: realtime=v1 header is no
        // longer accepted (the server returns "Realtime Beta API is no longer
        // supported").

        esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &Impl::websocket_event_trampoline, this);

        session_updated_ = false;
        pending_tool_call_ = false;
        audio_seq_ = 0;
        set_state(ConversationState::Connecting);

        // Spin up the audio sender pipeline. Has to be ready before the
        // first push_audio() call from conv-task, which fires within a few
        // ms of WEBSOCKET_EVENT_CONNECTED.
        audio_tx_queue_ = xQueueCreate(kAudioTxQueueLen, sizeof(AudioChunk*));
        if (audio_tx_queue_ == nullptr) {
            teardown();
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        sender_should_exit_.store(false, std::memory_order_relaxed);
        // Allocate the sender's 6 KiB stack from PSRAM via WithCaps — keeps
        // internal RAM available for mbedtls's transient RSA / AES contexts
        // during TLS handshake (those can be 8-16 KiB each).
        if (xTaskCreatePinnedToCoreWithCaps(&Impl::sender_trampoline, "rt_audio_tx",
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
            // Not an error: the coordinator may still be draining the mic
            // after speech_stopped. Drop silently.
            ESP_LOGD(kTag, "push_audio dropped (state != Listening)");
            return {};
        }
        if (pcm.empty()) {
            return {};
        }
        if (audio_tx_queue_ == nullptr) {
            return tl::unexpected{ConversationError::NotConnected};
        }

        // Encode PCM16 → G.711 µ-law into a PSRAM-resident chunk. The wire
        // form is one byte per sample (vs two for raw PCM16) and OpenAI
        // Realtime input is fixed at 8 kHz for pcmu — the caller supplies
        // mic samples at 8 kHz already (see kMicSampleRate in conv-task).
        // Keeping the chunks in PSRAM matters because the queue can hold a
        // few seconds of audio during a network stall.
        const std::size_t alloc = sizeof(AudioChunk) + pcm.size();
        auto* chunk = static_cast<AudioChunk*>(
            heap_caps_malloc(alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (chunk == nullptr) {
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        chunk->len_bytes = static_cast<std::uint16_t>(pcm.size());
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            chunk->data[i] = pcm16_to_ulaw(pcm[i]);
        }

        // Non-blocking enqueue. If the sender is stuck (network hiccup) and
        // the ring is full, drop the OLDEST chunk to keep the audio fresh —
        // realtime voice gets less useful with every second of latency. Mic
        // capture continues uninterrupted either way.
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
            note_tx_eviction();
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

    std::uint32_t tx_evicted_chunks() const { return tx_evicted_total_.load(std::memory_order_relaxed); }

private:
    // ---- lifecycle ---------------------------------------------------------

    // Count a mic-uplink eviction and emit at most one warning per
    // kEvictLogIntervalMs — under sustained congestion push_audio evicts on
    // every chunk, and a per-chunk ESP_LOGW floods the serial log. The
    // cumulative total is published per turn via tx_evicted_chunks(). Runs
    // only on the push_audio caller's task, so last_evict_log_ms_ needs no
    // synchronisation; the total is atomic because it is read cross-task.
    void note_tx_eviction()
    {
        static constexpr std::uint32_t kEvictLogIntervalMs = 5000;
        const std::uint32_t total = tx_evicted_total_.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        if (last_evict_log_ms_ == 0 || now_ms - last_evict_log_ms_ >= kEvictLogIntervalMs) {
            ESP_LOGD(kTag, "audio tx queue full; evicting oldest chunks (%u evicted this session)",
                     static_cast<unsigned>(total));
            last_evict_log_ms_ = now_ms;
        }
    }

    void teardown()
    {
        // Stop the sender first so it can't issue more writes against a
        // client we're about to destroy. Push a nullptr sentinel to unblock
        // a sender waiting on xQueueReceive.
        if (sender_task_ != nullptr) {
            sender_should_exit_.store(true, std::memory_order_release);
            AudioChunk* sentinel = nullptr;
            xQueueSend(audio_tx_queue_, &sentinel, 0);
            // Sender will self-delete; poll briefly for it to exit.
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
        // Release the PSRAM-backed scratch storage too — assign(0) keeps the
        // capacity, so use shrink_to_fit after clearing. Free the heap so a
        // subsequent reconnect doesn't leak it if the new chunk size differs.
        b64_scratch_.clear();
        b64_scratch_.shrink_to_fit();
        json_scratch_.clear();
        json_scratch_.shrink_to_fit();
        rx_capacity_ = 0;
        rx_len_ = 0;
        set_state(ConversationState::Idle);
    }

    // ---- audio sender task -------------------------------------------------

    static void sender_trampoline(void* arg)
    {
        static_cast<Impl*>(arg)->sender_loop();
        // Matches xTaskCreatePinnedToCoreWithCaps: frees the PSRAM-resident
        // stack as well as the TCB.
        vTaskDeleteWithCaps(nullptr);
    }

    void sender_loop()
    {
        AudioChunk* chunk = nullptr;
        while (!sender_should_exit_.load(std::memory_order_acquire)) {
            // Block with a small cap so we wake periodically to re-check the
            // exit flag even if the queue is idle.
            if (xQueueReceive(audio_tx_queue_, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
                continue;
            }
            if (chunk == nullptr) {
                break; // teardown sentinel
            }
            send_one_chunk(chunk);
            heap_caps_free(chunk);
            chunk = nullptr;
        }
    }

    // Encode + send a single chunk. Failures (WS aborted, encode error) are
    // logged but otherwise eaten — the WS event handler will surface the
    // disconnect through the normal Error path and conv-task drives the
    // reconnect. The mic side keeps producing in the meantime; the next
    // teardown will flush whatever is left in the queue.
    void send_one_chunk(AudioChunk* chunk)
    {
        if (client_ == nullptr) return;
        if (!esp_websocket_client_is_connected(client_)) return;

        const std::uint8_t* raw_ptr = chunk->data;
        const std::size_t raw_len = chunk->len_bytes;
        const std::size_t needed_b64 = base64::encoded_size(raw_len);
        if (needed_b64 > b64_scratch_.size()) {
            // Shouldn't happen — start() pre-sizes for the largest chunk —
            // but skip rather than try to grow on the hot path.
            return;
        }
        auto enc = base64::encode_into({raw_ptr, raw_len}, b64_scratch_);
        if (!enc) {
            return;
        }
        const int n = std::snprintf(json_scratch_.data(), json_scratch_.size(),
                                    "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%.*s\"}",
                                    static_cast<int>(*enc), b64_scratch_.data());
        if (n <= 0 || static_cast<std::size_t>(n) >= json_scratch_.size()) {
            return;
        }

        const std::uint32_t t0 =
            static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        int send_rc;
        {
            std::lock_guard lock{send_mutex_};
            send_rc = esp_websocket_client_send_text(client_, json_scratch_.data(), n, kSendTimeout);
        }
        const std::uint32_t dt =
            static_cast<std::uint32_t>(esp_timer_get_time() / 1000) - t0;
        ++audio_seq_;
        if (send_rc <= 0) {
            // esp_websocket_client already called abort_connection() — the
            // DISCONNECTED event will fire and conv-task reconnects. We log
            // for visibility but don't propagate.
            ESP_LOGW(kTag, "audio send failed seq=%lu dt=%lums size=%dB rc=%d",
                     static_cast<unsigned long>(audio_seq_),
                     static_cast<unsigned long>(dt), n, send_rc);
        } else if (dt >= 100 || (audio_seq_ % 100) == 1) {
            // Debug-only heartbeat — per-send lines flood the log on a
            // congested uplink; aggregates live in the per-turn metrics.
            const UBaseType_t queued = uxQueueMessagesWaiting(audio_tx_queue_);
            ESP_LOGD(kTag, "audio send seq=%lu dt=%lums queued=%u",
                     static_cast<unsigned long>(audio_seq_),
                     static_cast<unsigned long>(dt),
                     static_cast<unsigned>(queued));
        }
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
        // rc==0 means the poll_write timed out (nothing sent). Serialized JSON
        // is always non-empty here, so only rc>0 is a real success — treating 0
        // as success would silently drop tool results and hang the turn.
        return rc > 0;
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
        // Mic uplink is G.711 µ-law @ 8 kHz — 6× lighter on the wire than
        // pcm16 @ 24 kHz, which made the audio path immune to coex /
        // DTIM stalls eating its TCP send buffer. The mic side encodes
        // PCM16 → µ-law in push_audio (see pcm16_to_ulaw). OpenAI Realtime
        // accepts pcmu only at 8 kHz, so no rate field.
        cJSON_AddStringToObject(in_fmt, "type", "audio/pcmu");

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
    // TEMP: per-chunk sequence counter for the push_audio diagnostic log.
    std::uint32_t audio_seq_{0};
    bool pending_tool_call_{false};
    bool output_is_ulaw_{false};            // wire codec for assistant audio (g711_ulaw vs pcm16)
    std::atomic<bool> response_active_{false}; // a response is mid-generation (set/cleared from the WS task)

    // RX frame reassembly (PSRAM).
    char* rx_buffer_{nullptr};
    std::size_t rx_capacity_{0};
    std::size_t rx_len_{0};
    std::uint8_t rx_op_code_{0};

    // Hot-path scratch for input_audio_buffer.append. Storage lives in PSRAM
    // (via PsramAllocator) so the ~6 KiB combined doesn't eat the internal
    // heap that mbedtls needs for handshake-time allocations. Only the sender
    // task reads/writes these (push_audio just memcpys into PSRAM-allocated
    // chunks now), so no extra locking beyond send_mutex_ is needed for the
    // WS call.
    std::vector<char, PsramAllocator<char>> b64_scratch_;
    std::vector<char, PsramAllocator<char>> json_scratch_;

    // Audio tx pipeline: push_audio() enqueues PSRAM-resident AudioChunks
    // here, sender_loop() drains them and feeds esp_websocket_client_send_text.
    QueueHandle_t audio_tx_queue_{nullptr};
    // Session-cumulative eviction count, read cross-task by
    // tx_evicted_chunks() → conv-task metrics.
    std::atomic<std::uint32_t> tx_evicted_total_{0};
    std::uint32_t last_evict_log_ms_{0}; // note_tx_eviction rate limiter; 0 = never logged
    TaskHandle_t sender_task_{nullptr};
    std::atomic<bool> sender_should_exit_{false};

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

std::uint32_t OpenAiRealtimeClient::tx_evicted_chunks() const
{
    return impl_->tx_evicted_chunks();
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
