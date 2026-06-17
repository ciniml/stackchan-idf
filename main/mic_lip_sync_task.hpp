// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "shared_state.hpp"

namespace stackchan::app {

// Background task that drives `SharedState::mouth_open` from the mic input
// envelope. Spawned at boot only when BOTH the conversation backend and the
// jtts idle babble are disabled — i.e. the avatar would otherwise sit with a
// closed mouth and the I2S bus is free for the mic to own. While speaker
// playback is active (balloon say, MCP say, OTA chime, …) the task yields
// the I2S bus to the speaker and restarts mic capture once playback ends.
//
// No public stop API — the task lives for the rest of the boot. Re-enabling
// either flag takes effect on the Apply reboot, so this is fine for now.
void start_mic_lip_sync_task(SharedState& state);

} // namespace stackchan::app
