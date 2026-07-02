// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "wifi_config_service/mcp_events.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "sdkconfig.h"

namespace stackchan::wifi_config::mcp_events {

namespace {

constexpr const char* kTag = "mcp-events";

// Each queued event is rendered into a flat NUL-terminated SSE frame
// (`event: X\ndata: {...}\n\n`) so the streaming side just memcpy's onto
// the wire. 256 B fits the largest Phase 2 payload (boot, ~120 B with
// `firmware` + `ip` + `board`) with margin; depth 8 absorbs a brief stall
// without backpressuring publishers and without burning ~5 KB of internal
// RAM (queue storage lives in DRAM — see conversation_task.cpp's segment
// buffer alloc which fights for the same pool).
constexpr std::size_t kFrameBytes = 256;
constexpr std::size_t kQueueDepth = 8;

struct Frame {
    char buf[kFrameBytes];
    std::size_t len;
};

QueueHandle_t g_queue = nullptr;
std::atomic<bool> g_started{false};
ConvStatusGetter g_conv_getter = nullptr;

// One subscriber at a time. The id increments on each new subscriber;
// the event-stream loop bails out when its captured id no longer matches
// the current one, which is how we hand off cleanly if a second adapter
// connects mid-stream.
std::atomic<std::uint32_t> g_subscriber_id{0};

void format_frame(std::string_view type, std::string_view payload, Frame& out)
{
    // `payload` is the inner JSON body (without braces) so the publisher
    // can stay schema-light. We wrap with braces here.
    const int n = std::snprintf(out.buf, kFrameBytes,
                                "event: %.*s\ndata: {%.*s}\n\n",
                                static_cast<int>(type.size()), type.data(),
                                static_cast<int>(payload.size()), payload.data());
    if (n < 0) {
        out.len = 0;
        return;
    }
    out.len = static_cast<std::size_t>(n) < kFrameBytes
                  ? static_cast<std::size_t>(n)
                  : kFrameBytes - 1;
}

// Mirrors main/shared_state.hpp:ConvStatus enum order. Out-of-range values
// produce "unknown" so a future expansion of the enum doesn't crash the
// adapter — it just sees an opaque label until the firmware → adapter
// versions catch up.
const char* conv_status_name(int s)
{
    switch (s) {
    case 0: return "disabled";
    case 1: return "waiting_wifi";
    case 2: return "connecting";
    case 3: return "listening";
    case 4: return "talking";
    case 5: return "yielded";
    case 6: return "reconnecting";
    case 7: return "error";
    default: return "unknown";
    }
}

void monitor_task_entry(void* /*arg*/)
{
    if (g_conv_getter == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    int last = g_conv_getter();
    // 250 ms is short enough to feel responsive for a state change Claude
    // is going to relay to the user and long enough to keep CPU + queue
    // pressure trivial.
    constexpr TickType_t kPeriod = pdMS_TO_TICKS(250);
    for (;;) {
        const int cur = g_conv_getter();
        if (cur != last) {
            char payload[64];
            const int n = std::snprintf(payload, sizeof(payload),
                                        "\"state\":\"%s\"", conv_status_name(cur));
            if (n > 0) {
                publish("conversation_state",
                        std::string_view{payload, static_cast<std::size_t>(n)});
            }
            last = cur;
        }
        vTaskDelay(kPeriod);
    }
}

// SSE worker task body. Runs on its own task (spawned by run_stream) so
// the esp_http_server worker thread is free to handle /mcp/say etc. while
// the long-poll is alive. Must NOT touch `req` after
// httpd_req_async_handler_complete().
void sse_worker_task(void* arg)
{
    auto* req = static_cast<httpd_req_t*>(arg);
    const std::uint32_t my_id = ++g_subscriber_id;

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    // Disable any reverse-proxy buffering. Cloudflare Tunnel respects this
    // and flushes per chunk, which is what makes SSE actually feel pushed.
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

    // Prelude: comment frame so the client confirms the stream is open
    // before the first real event lands. Without this, curl shows nothing
    // until the first event arrives, which is confusing during testing.
    constexpr char kHello[] = ": stackchan mcp-events stream open\n\n";
    bool alive = (httpd_resp_send_chunk(req, kHello, sizeof(kHello) - 1) == ESP_OK);

    // 15 s heartbeat tick. Cloudflare Tunnel idle-closes around 100 s with
    // no traffic, and even a busy session benefits from regular liveness
    // pings (so a half-open TCP shows up promptly as a chunk-send failure).
    constexpr TickType_t kReceiveTimeout = pdMS_TO_TICKS(15'000);
    while (alive && g_subscriber_id.load(std::memory_order_acquire) == my_id) {
        Frame f{};
        if (xQueueReceive(g_queue, &f, kReceiveTimeout) == pdTRUE) {
            if (httpd_resp_send_chunk(req, f.buf, f.len) != ESP_OK) break;
        } else {
            constexpr char kKeep[] = ": keepalive\n\n";
            if (httpd_resp_send_chunk(req, kKeep, sizeof(kKeep) - 1) != ESP_OK) break;
        }
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    httpd_req_async_handler_complete(req);
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t run_stream(httpd_req_t* req)
{
    if (g_queue == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    // Hand the request off to a dedicated task — without this, the worker
    // thread that called us blocks on send_chunk for the entire SSE lifetime
    // and esp_http_server can't accept ANY other request (it has a single
    // worker by design, even with max_open_sockets > 1). After
    // async_handler_begin, the original worker returns immediately while the
    // task we spawn owns the response.
    httpd_req_t* async_req = nullptr;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "async_handler_begin failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // 4 KiB internal-RAM stack. The worker only does small snprintfs +
    // httpd_resp_send_chunk; the heavy lifting (TLS, etc.) is in lwip/httpd
    // helper tasks. Pin to core 0 alongside the other httpd-adjacent work.
    if (xTaskCreatePinnedToCore(sse_worker_task, "sse-worker", 4096, async_req,
                                tskIDLE_PRIORITY + 1, nullptr, 0) != pdPASS) {
        ESP_LOGE(kTag, "sse_worker_task create failed");
        httpd_req_async_handler_complete(async_req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void start(ConvStatusGetter getter)
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    g_conv_getter = getter;
    // Queue storage lives in PSRAM (saves ~2 KiB of internal RAM that
    // conversation_task's segment buffers + TLS handshake fight over at
    // boot). Only the queue control block stays in DRAM, which is
    // unavoidable — FreeRTOS uses it from ISR context.
    static StaticQueue_t s_queue_cb;
    auto* storage = static_cast<std::uint8_t*>(
        heap_caps_malloc(kQueueDepth * sizeof(Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (storage == nullptr) {
        ESP_LOGE(kTag, "queue storage alloc (PSRAM) failed");
        g_started.store(false, std::memory_order_relaxed);
        return;
    }
    g_queue = xQueueCreateStatic(kQueueDepth, sizeof(Frame), storage, &s_queue_cb);
    if (g_queue == nullptr) {
        ESP_LOGE(kTag, "xQueueCreateStatic failed");
        heap_caps_free(storage);
        g_started.store(false, std::memory_order_relaxed);
        return;
    }
    if (getter != nullptr) {
        // 4 KiB. The task body is tiny (atomic load + snprintf + publish),
        // but the publish path uses a 264 B Frame on stack + snprintf's
        // vfprintf (~500 B newlib). 3 KiB blew up during GC0308 camera init
        // (~38 s) when an I2C-driven ISR landed on top of a state-change
        // publish — the IRQ frame + interrupted vfprintf together breached
        // the canary. 2 KiB had already been ruled out at boot.
        xTaskCreatePinnedToCore(monitor_task_entry, "mcp-evt-mon", 4096, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr, 0);
    }
}

void publish(std::string_view type, std::string_view payload)
{
    if (g_queue == nullptr) return;
    Frame f{};
    format_frame(type, payload, f);
    if (f.len == 0) return;
    // Drop on full so a stalled adapter can't backpressure the publishers.
    (void)xQueueSend(g_queue, &f, 0);
}

void publish_boot(std::string_view firmware_version, std::string_view ip,
                  std::uint8_t board_kind)
{
    char payload[160];
    const int n = std::snprintf(payload, sizeof(payload),
                                "\"firmware\":\"%.*s\",\"ip\":\"%.*s\",\"board\":%u",
                                static_cast<int>(firmware_version.size()), firmware_version.data(),
                                static_cast<int>(ip.size()), ip.data(),
                                static_cast<unsigned>(board_kind));
    if (n > 0) {
        publish("boot", std::string_view{payload, static_cast<std::size_t>(n)});
    }
}

void publish_touch_stroke(std::string_view direction)
{
    char payload[64];
    const int n = std::snprintf(payload, sizeof(payload),
                                "\"direction\":\"%.*s\"",
                                static_cast<int>(direction.size()), direction.data());
    if (n > 0) {
        publish("touch", std::string_view{payload, static_cast<std::size_t>(n)});
    }
}

void publish_say_done()
{
    publish("say_done", "");
}

} // namespace stackchan::wifi_config::mcp_events
