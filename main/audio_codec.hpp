// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <vector>

namespace stackchan::app {

// AAC-encoded recording (ADTS-framed).
struct AacRecording {
    std::vector<std::uint8_t> data;
    std::uint32_t sample_rate{16000};
    std::uint8_t channels{1};
};

// Records `seconds` seconds from M5.Mic and encodes the result as AAC.
// PCM is held in PSRAM transiently; only the compressed bytestream remains
// in `out`. Returns true on success.
bool record_aac(std::uint32_t seconds, AacRecording& out);

// Decodes `recording` back to PCM and plays it through M5.Speaker.
// Blocks until playback finishes.
bool play_aac(const AacRecording& recording);

} // namespace stackchan::app
