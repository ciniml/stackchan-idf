// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <memory>
#include <string>

#include "conversation/conversation_service.hpp"

namespace stackchan::conversation {

// ConversationService backed by the Google Gemini Live API over a WebSocket
// (wss://generativelanguage.googleapis.com/.../BidiGenerateContent?key=...).
// Server-side VAD by default — commit_audio() is a no-op. The mic uplink
// must be PCM16 mono @ 16 kHz; the assistant reply comes back as PCM16 mono
// @ 24 kHz (forwarded straight to AssistantAudioChunk listeners, no
// resampling done in the client).
//
// Sessions cap at ~15 min of audio in default config; the server emits a
// goAway warning before disconnecting and an opaque sessionResumption
// handle which this client caches and replays in the next start() so the
// conversation feels continuous to the user across the forced reconnect.
class GeminiLiveClient final : public ConversationService {
public:
    // `api_key` is a Google AI Studio API key. Authentication is via URL
    // query parameter (`?key=...`); no HTTP header is sent on the upgrade.
    explicit GeminiLiveClient(std::string api_key);
    ~GeminiLiveClient() override;

    GeminiLiveClient(const GeminiLiveClient&) = delete;
    GeminiLiveClient& operator=(const GeminiLiveClient&) = delete;

    void set_event_callback(EventCallback cb) override;
    tl::expected<void, ConversationError> start(const ConversationConfig& config) override;
    void stop() override;
    tl::expected<void, ConversationError> push_audio(std::span<const std::int16_t> pcm) override;
    tl::expected<void, ConversationError> commit_audio() override;
    tl::expected<void, ConversationError> submit_tool_result(std::string_view call_id,
                                                             std::string_view output_json) override;
    tl::expected<void, ConversationError> cancel_response() override;
    ConversationState state() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stackchan::conversation
