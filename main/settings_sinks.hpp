// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once
#include <string_view>

#include <cstdint>

#include <jtts/jtts.hpp>

#include "shared_state.hpp"

// Settings sink/getter wiring shared by the BLE (config_service) and HTTP
// (wifi_config_service) settings transports, plus the /mcp/* channel sinks.
//
// Everything here is the glue between "a settings client wrote a value" and
// "the running firmware reflects it": the sinks mutate SharedState atomics
// (live-apply) and persist via the single-writer config_store save_* helpers;
// the getters read the same atomics back so both transports serve identical
// values. app_main calls the register_* functions at the exact boot points
// the corresponding services come online (BLE before config::start, HTTP
// right after wifi_start, MCP after the conversation task exists).
namespace stackchan::app::settings_sinks {

// Late-bound SharedState. The speaker-volume path runs before SharedState
// exists (boot arpeggio), so every function here tolerates a null state;
// attach_state() flips them live. Call once, before any register_*().
void attach_state(SharedState& state);

// kana (ひらがな) を発話タスクで喋る。ASR ウェイクワードの返事等から使う。
void say_kana(std::string_view kana_utf8);

// Per-board factory speaker volume — the "100 %" reference the user gain
// multiplies (see apply_speaker_volume). Set from boot before the first
// apply_speaker_volume call.
void set_speaker_base_volume(std::uint8_t base);

// jtts options used by the settings-page test-speak buttons (BLE chr 0x2d +
// /api/jtts-say) and /mcp/say. Cached from cfg.jtts_config_json at boot so
// the test voice matches the demo_loop babble voice.
void set_say_options(const stackchan::jtts::Options& opts);

// Live-apply the user's speaker_volume_pct (0..200) to the M5.Speaker
// master volume and mirror into SharedState (when attached). No NVS write —
// the caller handles persistence. One-touch mute overrides the percent to 0
// while SharedState::speaker_muted is set.
void apply_speaker_volume(std::uint16_t pct) noexcept;

// apply_speaker_volume + persist via config_store::save_speaker_volume.
// This is the sink both settings transports and demo_loop's volume watcher
// use, so every path takes the same setVolume + NVS route.
void apply_speaker_volume_persist(std::uint16_t pct);

// Sink/getter registration batches. Split by transport so app_main can keep
// the original boot ordering (BLE sinks must land before config::start; the
// HTTP ones after wifi_start; avatar-bytecode and MCP at their later points).
void register_ble_sinks(std::uint8_t board_kind);
void register_http_sinks(std::uint8_t board_kind);
void register_avatar_bytecode_sinks();
void register_mcp_sinks();

} // namespace stackchan::app::settings_sinks
