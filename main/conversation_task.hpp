// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "board/si12t_touch.hpp"
#include "config_service/config_service.hpp"
#include "shared_state.hpp"

namespace stackchan::app {

struct ConversationTaskArgs {
    SharedState* state;
    const char* api_key;          // API key for the chosen provider; empty disables the task
    config::Provider provider;    // selects OpenAI Realtime vs. Gemini Live
    board::Si12tTouch* touch;     // top touch sensor for barge-in; may be null
};

// Pinned to core 0. Owns the chosen ConversationService backend and drives
// the half-duplex mic/speaker I2S state machine, avatar mouth sync, balloon
// text, and tool dispatch. Waits for Wi-Fi before connecting.
void start_conversation_task(ConversationTaskArgs& args);

} // namespace stackchan::app
