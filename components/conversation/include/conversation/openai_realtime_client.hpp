// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <memory>
#include <string>

#include "conversation/conversation_service.hpp"

namespace stackchan::conversation {

// ConversationService backed by the OpenAI Realtime API over a raw WebSocket
// (wss://api.openai.com/v1/realtime). Server-side VAD/STT/LLM/TTS — the whole
// pipeline runs in one session, so commit_audio() is a no-op here.
class OpenAiRealtimeClient final : public ConversationService {
public:
    // `api_key` is the OpenAI API key used for the WebSocket upgrade
    // (Authorization: Bearer ...).
    explicit OpenAiRealtimeClient(std::string api_key);
    ~OpenAiRealtimeClient() override;

    OpenAiRealtimeClient(const OpenAiRealtimeClient&) = delete;
    OpenAiRealtimeClient& operator=(const OpenAiRealtimeClient&) = delete;

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
