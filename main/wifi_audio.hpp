// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "shared_state.hpp"

namespace stackchan::app::wifi_audio {

// Start the Wi-Fi live audio receiver: an RTP listener on a fixed UDP port
// that depacketizes (RTP/L16 PCM today, RTP/AAC later) into mono S16, plays
// it through M5.Speaker on a low-latency jitter buffer, and drives
// `state.face.mouth_open` from the chunk RMS — the same speaker / mouth / I2S
// handoff the BLE audio sink uses.
//
// `rtp_enabled` reflects cfg.rtp_audio_enabled — the master on/off switch the
// user sets from either settings page. When false the receiver is not started.
//
// `conversation_enabled` reflects cfg.openai_enabled. Like the BLE sink,
// Wi-Fi audio and the realtime conversation backend are mutually exclusive
// (they contend for the I2S bus + CPU), so the receiver is also not started
// when conversation mode is on.
//
// Must be called after Board::begin() (M5.Speaker ready) and after wifi_start();
// the receiver task waits for the Wi-Fi link before binding its socket.
void start(SharedState& state, bool conversation_enabled, bool rtp_enabled);

} // namespace stackchan::app::wifi_audio
