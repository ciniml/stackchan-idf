// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <string>

namespace stackchan::app {

class Speech;
class SharedState;

// LT (lightning talk) timekeeper. Owned and ticked by demo_loop — the same
// task that owns Speech / the speaker — so announcements can synthesise
// synchronously without fighting other speakers. The on-device LT tab (and
// later BLE/HTTP) communicates through SharedState atomics:
//   lt_command     UI → timer: 1 = start, 2 = stop/reset
//   lt_total_s     UI → timer: talk length (read once at start)
//   lt_active      timer → UI
//   lt_remaining_s timer → UI (signed; negative = overtime)
class LtTimer {
public:
    struct Config {
        std::uint32_t warn_s = 60;    // "remaining" announcement threshold
        std::uint32_t soon_s = 15;    // shorter "almost there" threshold
        std::uint32_t repeat_s = 30;  // overtime re-announcement period
        // Announcement texts: display goes to the balloon (free-form UTF-8),
        // reading is the kana fed to jtts (no kanji dictionary).
        std::string warn_display = "のこり1分です";
        std::string warn_reading = "のこり いっぷん です";
        // Short-warning at soon_s before the deadline. Empty → silent
        // (no announcement at this threshold, only warn and just/over
        // remain). Default text is set non-empty so a fresh install gets
        // the 15-second nudge out of the box.
        std::string soon_display = "のこり15秒です";
        std::string soon_reading = "のこり じゅうごびょう です";
        // Exact-deadline announcement (fires once when remaining_ms first
        // crosses 0). Empty → fall back to over_* so older saved configs
        // hear the same thing they always did.
        std::string just_display;
        std::string just_reading;
        std::string over_display = "時間です!";
        std::string over_reading = "おじかん です";
    };

    // Compact JSON override (NVS-persisted, BLE/HTTP-settable):
    //   {"total_s":300,"warn_s":60,"repeat_s":30,
    //    "warn":{"text":"のこり1分です","reading":"のこり いっぷん です"},
    //    "over":{"text":"時間です!","reading":"おじかん です"}}
    // Missing / invalid fields keep their defaults; invalid JSON is ignored.
    // `total_s` (the default talk length) lives in SharedState because the
    // on-device presets also write it — pass `state` to apply it (skipped
    // while a talk is running so a live countdown can't be yanked).
    void configure(const std::string& json, SharedState* state = nullptr);

    // Call every demo_loop iteration (~50 ms cadence). Consumes commands,
    // updates the published state, and speaks announcements through
    // `speech` (which also drives the avatar's mouth).
    void tick(SharedState& state, Speech& speech, std::uint32_t now_ms);

private:
    void announce(SharedState& state, Speech& speech,
                  const std::string& display, const std::string& reading);

    Config cfg_;
    bool running_ = false;
    bool warned_ = false;
    bool soon_fired_ = false;
    bool just_fired_ = false;
    std::uint32_t deadline_ms_ = 0;
    std::uint32_t next_over_ms_ = 0;
};

} // namespace stackchan::app
