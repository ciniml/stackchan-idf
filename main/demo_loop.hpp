// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <string>

#include "board/board.hpp"
#include "board/si12t_touch.hpp"
#include "servo_limits.hpp"
#include "shared_state.hpp"

// The main-task foreground loop: input dispatch (M5 buttons / LCD touch /
// IMU shake / Si12T nadenade) + idle avatar behaviours (random poses, jtts
// babble balloons, expression cycling) + housekeeping watchers (speaker
// volume/mute, LT timer, battery polling). Runs forever on the app_main
// task; every other subsystem lives in its own task.
namespace stackchan::app {

struct DemoLoopArgs {
    SharedState* state = nullptr;             // required
    stackchan::board::Board* board = nullptr; // for vibrate() / kind(); may be null in theory
    stackchan::board::Si12tTouch* touch = nullptr; // head sensor; null when absent
    std::string jtts_config_json;              // babble voice options
    bool has_battery = false;                  // poll INA226 every 5 s
    bool is_atom_nyan = false;                 // button overlay UI instead of LCD touch
    bool btn_a_toggles_ui = false;             // StopWatch: BtnA opens/closes device_ui
    bool touch_gaze_follow = false;            // StopWatch: outer-ring gaze following
    bool conversation_enabled = false;         // gates the Wi-Fi-disconnected balloon
    bool jtts_idle_enabled = false;            // idle babble + mouth envelope
    ServoLimits limits;                        // random-pose ranges
};

[[noreturn]] void run_demo_loop(const DemoLoopArgs& args);

} // namespace stackchan::app
