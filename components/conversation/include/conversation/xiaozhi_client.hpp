// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <memory>
#include <string>

#include "conversation/conversation_service.hpp"

namespace stackchan::conversation {

// ConversationService backed by a XiaoZhi AI server
// (xinnan-tech/xiaozhi-esp32-server) over its WebSocket transport
// (ws://<host>:<port>/xiaozhi/v1/). The server runs the whole pipeline
// (VAD / ASR / LLM / TTS), so this client just streams Opus audio both ways
// and relays the JSON control channel:
//   - mic uplink:  PCM16 mono @ 16 kHz → Opus (60 ms / 960-sample frames)
//   - reply downlink: Opus → PCM16 mono, resampled to the configured output
//     rate (the server announces its TTS sample rate in its hello).
//   - control: `stt` → UserTranscript, `tts` start/sentence_start/stop drive
//     the turn, `llm` emotion → AssistantEmotion (avatar expression).
//
// Server-side VAD (listen mode=auto), so commit_audio() is a no-op. v1 carries
// no MCP / custom tools, so submit_tool_result() is a documented no-op.
class XiaoZhiClient final : public ConversationService {
public:
    // `url` is the full WebSocket endpoint (e.g.
    // "ws://192.168.1.10:8000/xiaozhi/v1/"). `token` rides in the
    // "Authorization: Bearer <token>" upgrade header; an empty token omits
    // the header (servers that don't require auth). The Device-Id /
    // Client-Id upgrade headers are derived from the Wi-Fi STA MAC inside
    // the client.
    XiaoZhiClient(std::string url, std::string token);
    ~XiaoZhiClient() override;

    XiaoZhiClient(const XiaoZhiClient&) = delete;
    XiaoZhiClient& operator=(const XiaoZhiClient&) = delete;

    void set_event_callback(EventCallback cb) override;
    tl::expected<void, ConversationError> start(const ConversationConfig& config) override;
    void stop() override;
    tl::expected<void, ConversationError> push_audio(std::span<const std::int16_t> pcm) override;
    tl::expected<void, ConversationError> commit_audio() override;
    tl::expected<void, ConversationError> submit_tool_result(std::string_view call_id,
                                                             std::string_view output_json) override;
    tl::expected<void, ConversationError> cancel_response() override;
    ConversationState state() const override;
    std::uint32_t tx_evicted_chunks() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stackchan::conversation
