// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "shared_state.hpp"

namespace stackchan::app::audio_stream {

// Initialise the BLE audio streaming sink and register it with
// config_service. Spawns a worker task that owns the AAC decoder, drains
// incoming ADTS bytes, plays the decoded PCM through M5.Speaker, and
// updates `state.face.mouth_open` from the chunk RMS.
//
// `conversation_enabled` reflects cfg.openai_enabled: when the realtime
// conversation backend is active it contends with BLE for the radio/CPU
// and makes streaming playback choppy, so a `begin` is rejected outright
// in that mode (BLE audio + voice chat are mutually exclusive).
//
// Must be called after Board::begin() so M5.Speaker is initialised.
void start(SharedState& state, bool conversation_enabled);

} // namespace stackchan::app::audio_stream
