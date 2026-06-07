// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

// Push-event surface for the Claude Code Channel adapter. Subsystems
// publish discrete events (touch, say_done, conversation_state, boot, ...)
// into a small in-memory FIFO; the GET /mcp/events HTTP handler streams
// them out as Server-Sent Events to the (single) adapter subscriber.
//
// Wire format on `/mcp/events` (text/event-stream):
//
//     event: <type>
//     data: <one-line JSON object>
//     <blank line>
//
// And periodic `: keepalive` comment frames every 15 s so Cloudflare
// Tunnel doesn't drop the idle connection.

#include <cstdint>
#include <string_view>

#include "esp_http_server.h"

namespace stackchan::wifi_config::mcp_events {

// Returns the current conversation-status enum cast to int, in the same
// order as main/shared_state.hpp's ConvStatus enum (0=Disabled,
// 1=WaitingWifi, 2=Connecting, 3=Listening, 4=Talking, 5=Yielded,
// 6=Reconnecting, 7=Error). Anything outside that range is reported as
// "unknown" downstream.
//
// We take this as a callback rather than including shared_state.hpp because
// shared_state.hpp lives in main/ — wifi_config_service can't depend on it
// without making the build graph circular (main already REQUIRES us).
using ConvStatusGetter = int (*)();

// Spawn the background monitor task (watches conversation status for diffs
// against the previous tick) and create the event FIFO. Idempotent. Call
// ONCE from app_main after SharedState is constructed; safe to call before
// the HTTP server is up. `getter` may be nullptr → conversation_state
// events are never published, but the rest of the wire still works.
void start(ConvStatusGetter getter);

// Stream events as Server-Sent Events on `req` until the client disconnects
// (or a newer adapter replaces this subscriber). Returns ESP_OK. Caller is
// expected to have already verified Bearer auth — this function does not
// re-check. Designed to be invoked from a small wrapper in http_handlers.cpp
// that handles the /mcp/* auth gate.
esp_err_t run_stream(httpd_req_t* req);

// Thread-safe enqueue. `type` is the SSE event name (e.g. "touch"); `payload`
// is the body of a one-line JSON object WITHOUT the surrounding braces
// (e.g. `"x":120,"y":80`). Drops the event silently if the FIFO is full —
// the SSE consumer is presumed to be slow at worst, not absent, and Phase 2
// doesn't try to replay missed events.
void publish(std::string_view type, std::string_view payload);

// Convenience publishers for the Phase 2 wired events.
void publish_boot(std::string_view firmware_version, std::string_view ip,
                  std::uint8_t board_kind);
// direction: "front_to_back" | "back_to_front" — matches the nadenade
// detector's two completion variants in app_main.cpp.
void publish_touch_stroke(std::string_view direction);
void publish_say_done();
// conversation_state is published internally by the monitor task on change.

} // namespace stackchan::wifi_config::mcp_events
