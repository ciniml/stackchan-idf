// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "demo_loop.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <wifi_config_service/mcp_events.hpp>
#include <wifi_config_service/wifi_config_service.hpp>
#include "config_service/config_service.hpp"

#include "ap_screen.hpp"
#include "atom_status.hpp"
#include "avatar/expression.hpp"
#include "battery.hpp"
#include "device_ui.hpp"
#include "lt_timer.hpp"
#include "settings_sinks.hpp"
#include "speech.hpp"
#include "wifi_sta.hpp"

namespace stackchan::app {

namespace {
constexpr const char* kTag = "stackchan";
} // namespace

[[noreturn]] void run_demo_loop(const DemoLoopArgs& args)
{
    // Aliases keep the body textually identical to its pre-extraction form
    // in app_main.cpp (where these were file-scope globals / parameters).
    SharedState* const g_state = args.state;
    stackchan::board::Board* const g_board = args.board;
    stackchan::board::Si12tTouch* const g_touch = args.touch;
    const std::string& jtts_config_json = args.jtts_config_json;
    const bool has_battery = args.has_battery;
    const bool is_atom_nyan = args.is_atom_nyan;
    const bool btn_a_toggles_ui = args.btn_a_toggles_ui;
    const bool touch_gaze_follow = args.touch_gaze_follow;
    const bool conversation_enabled = args.conversation_enabled;
    const bool jtts_idle_enabled = args.jtts_idle_enabled;
    const ServoLimits& limits = args.limits;

    using namespace stackchan;

    constexpr avatar::Expression kCycle[] = {
        avatar::Expression::Neutral, avatar::Expression::Happy, avatar::Expression::Doubt,
        avatar::Expression::Sad,     avatar::Expression::Angry, avatar::Expression::Sleepy,
    };

    // Random head pose targets, redrawn every kPoseMinMs..kPoseMaxMs. The
    // ranges come from the per-device ServoLimits so the demo respects the
    // configured motion (servo_task also clamps defensively).
    const float kYawMinDeg = static_cast<float>(limits.yaw_min_deg);
    const float kYawMaxDeg = static_cast<float>(limits.yaw_max_deg);
    const float kPitchMinDeg = static_cast<float>(limits.pitch_min_deg);
    const float kPitchMaxDeg = static_cast<float>(limits.pitch_max_deg);
    constexpr std::uint32_t kPoseMinMs = 10000;
    constexpr std::uint32_t kPoseMaxMs = 20000;
    constexpr std::uint32_t kExpressionPeriodMs = 5000;
    constexpr std::uint32_t kSpeechMinMs = 6000;
    constexpr std::uint32_t kSpeechMaxMs = 12000;

    static app::Speech speech;
    speech.configure(jtts_config_json);

    // LT timekeeper — ticked every loop iteration; speaks through the same
    // Speech instance (so the avatar's mouth moves) and publishes state for
    // the on-device LT tab. configure() is fed later from NVS (Phase 4).
    static app::LtTimer lt_timer;

    auto rand_in = [](float low, float high) {
        const float u = static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX);
        return low + (high - low) * u;
    };
    auto rand_range_ms = [](std::uint32_t low, std::uint32_t high) {
        return low + (esp_random() % (high - low + 1));
    };

    std::size_t expression_index = 0;
    std::uint32_t next_expression_ms = 0;
    std::uint32_t next_pose_ms = 0;
    std::uint32_t next_speech_ms = 2000; // first babble shortly after boot

    // Base-board battery monitor (INA226 on the internal I2C bus). Read here —
    // the only task that touches m5::In_I2C — and published to SharedState +
    // the BLE / Wi-Fi services. Only the M5 base has the INA226; on boards
    // without it (Takao) skip entirely, leaving battery_* = -1 ("—" everywhere).
    constexpr std::uint32_t kBatteryPeriodMs = 5000;
    app::BatteryMonitor battery;
    if (has_battery) {
        battery.begin();
    }
    std::uint32_t next_battery_ms = 0;

    // Nadenade (head-petting) detection on the top-mounted Si12T sensor.
    //
    // A static "is something touching?" test kept false-firing on 2.4 GHz
    // EMI. Captured sensor traces show the real discriminator is the *onset
    // order*: a real pet drags across the head, so each zone first reaches a
    // firm contact (intensity 3) in spatial order — front→middle→back, or the
    // reverse. (Untouched, the chip reads a clean 0 0 0; the zones overlap
    // heavily mid-stroke — front=3,middle=3 ties etc. — so tracking a single
    // "dominant" zone doesn't work; the first-hit timestamps do.)
    //
    // We trigger only when all three zones have hit intensity 3 within one
    // gesture AND their first-hit times are monotonic across the head, with
    // the two ends hit in *different* samples so a single uniform RFI spike
    // (all three at once) can't qualify.
    //   - kStrokePeakIntensity: a zone counts as "hit" at this intensity (3).
    //   - kStrokeGapMs: an all-quiet stretch this long ends the gesture.
    constexpr std::uint8_t kStrokePeakIntensity = 3;
    constexpr std::uint32_t kStrokeGapMs = 600;
    constexpr std::uint32_t kNadenadeCooldownMs = 4000;
    std::array<std::uint32_t, 3> stroke_hit_ms{0, 0, 0}; // first-hit-3 time per zone (0 = not yet)
    std::uint32_t stroke_active_ms = 0;   // last time any zone was non-zero
    std::uint32_t next_nadenade_ms = 0;   // earliest time we'll trigger again

    // Last-applied speaker volume percent. Watches the SharedState atom
    // so the device-UI's −/+ nudge buttons re-apply via the same
    // setVolume + NVS path the BLE / WiFi sinks use. Seeded from the
    // current atom (set by boot's apply_speaker_volume_sink call).
    std::uint16_t last_speaker_volume_pct =
        g_state->speaker.volume_pct.load(std::memory_order_relaxed);
    // One-touch mute edge detection (corner tap / BtnA hold). Applied via
    // apply_speaker_volume (NOT the sink) — mute is session-only, no NVS.
    bool last_speaker_muted = g_state->speaker.muted.load(std::memory_order_relaxed);

    // Set true by the (render-task) completion callback so demo_loop knows
    // the previous balloon finished. Atomics keep it thread-safe.
    static std::atomic<bool> balloon_in_flight{false};

    // Wi-Fi state edge detection: while disconnected we pin a persistent
    // "Wi-Fi: 切断中" balloon and suppress babble; when it reconnects we
    // clear the balloon so normal demo behaviour resumes.
    bool wifi_warning_active = false;

    // BMI270 shake → randomized expression. Cheap to poll (one I2C read);
    // the cooldown keeps a single jerk from cascading into rapid-fire
    // changes. Magnitude threshold is in g-units after subtracting 1 g of
    // gravity, so it ignores normal handheld motion and only fires on
    // deliberate flicks of the wrist. Available on any board M5Unified
    // configured an IMU for (StopWatch's BMI270 in this scope; harmless
    // no-op on CoreS3 where the IMU isn't initialised — getAccel returns
    // false and we skip).
    constexpr float kShakeThresholdG = 1.6f;       // |a| ≥ 1.6 g (≈ 0.6 g jerk)
    constexpr std::uint32_t kShakeCooldownMs = 800;
    std::uint32_t next_shake_ms = 0;

    for (;;) {
        // Camera session in progress: every In_I2C touch (M5.update's
        // touch/BtnPWR poll, INA226 battery, Si12T nadenade, BMI270 shake)
        // would re-init the I2C controller under the camera's SCCB driver
        // and kill the capture. Idle the whole iteration instead — sessions
        // are ~1.5 s one-shots, so buttons/touch just miss a beat.
        if (g_state->i2c_quiesce.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Drive M5.update() so M5.Touch / M5.BtnPWR latch their state machines.
        M5.update();

        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);

        // Live-apply speaker_volume_pct changes from the device-UI (BLE /
        // HTTP sinks already apply directly). Cheap atomic compare per
        // iteration; only triggers M5.Speaker.setVolume + NVS write on
        // an actual change.
        {
            const std::uint16_t cur = g_state->speaker.volume_pct.load(std::memory_order_relaxed);
            if (cur != last_speaker_volume_pct) {
                last_speaker_volume_pct = cur;
                settings_sinks::apply_speaker_volume_persist(cur);
            }
        }

        // One-touch mute toggles (device_ui corner tap / atom_status BtnA
        // hold). Re-run apply_speaker_volume with the unchanged percent so
        // the M5.Speaker master volume snaps to 0 / back immediately — this
        // also silences audio that's already mid-playback, since the mixer
        // applies master volume per chunk. A short haptic confirms the
        // toggle on boards with a vibration motor (no-op elsewhere).
        {
            const bool muted = g_state->speaker.muted.load(std::memory_order_relaxed);
            if (muted != last_speaker_muted) {
                last_speaker_muted = muted;
                settings_sinks::apply_speaker_volume(
                    g_state->speaker.volume_pct.load(std::memory_order_relaxed));
                ESP_LOGI(kTag, "speaker %s", muted ? "muted" : "unmuted");
                if (g_board != nullptr) (void)g_board->vibrate(20);
            }
        }

        // IMU shake → cycle to a random expression. Runs before the
        // touch/UI block so a shake during conv idle interrupts the
        // expression rotation immediately (no wait for the 5 s timer).
        // A brief haptic confirms the shake actually registered (handy
        // when the user can't see the avatar on a wrist-worn device).
        if (now_ms >= next_shake_ms) {
            float ax = 0, ay = 0, az = 0;
            if (M5.Imu.getAccel(&ax, &ay, &az)) {
                const float mag = std::sqrt(ax * ax + ay * ay + az * az);
                if (mag >= kShakeThresholdG) {
                    next_shake_ms = now_ms + kShakeCooldownMs;
                    const int cur = g_state->face.expression.load(std::memory_order_relaxed);
                    int next = cur;
                    for (int i = 0; i < 4 && next == cur; ++i) {
                        next = static_cast<int>(esp_random() % 6);
                    }
                    g_state->face.expression.store(next, std::memory_order_relaxed);
                    ESP_LOGI(kTag, "shake |a|=%.2fg → expression %d", mag, next);
                    if (g_board != nullptr) (void)g_board->vibrate(60);
                }
            }
        }
        // BtnB (StopWatch Blue / G1) — manual expression cycle with haptic
        // confirmation. wasPressed() is false on boards without BtnB so the
        // check is harmless universally.
        if (M5.BtnB.wasPressed()) {
            const int cur = g_state->face.expression.load(std::memory_order_relaxed);
            g_state->face.expression.store((cur + 1) % 6, std::memory_order_relaxed);
            if (g_board != nullptr) (void)g_board->vibrate(30);
        }
        // BtnA on StopWatch (= Yellow / G2) — toggle the device_ui open/close.
        // The corner tap-to-open hot zone on a round AMOLED is awkward to hit
        // (corners are within the visible circle but at the edge of the touch
        // pad), so a physical button is a more reliable opener. On other
        // boards BtnA either has no role (CoreS3 uses touch only) or is
        // already claimed by atom_status::poll_button (AtomS3R / AtomS3 →
        // is_atom_nyan branch above, which doesn't reach this path).
        if (btn_a_toggles_ui && g_board != nullptr && M5.BtnA.wasPressed()) {
            app::ui::toggle();
            (void)g_board->vibrate(20);
        }

        // LT timekeeper: re-configure when BLE/HTTP (or the boot seed) pushed
        // a new config JSON, then consume UI commands / update the countdown /
        // announce the 1-minute warning + overtime through speech + balloon.
        static std::uint32_t lt_cfg_seen = 0;
        if (const std::uint32_t v = g_state->lt_config_version(); v != lt_cfg_seen) {
            lt_cfg_seen = v;
            lt_timer.configure(g_state->snapshot_lt_config(), g_state);
        }
        lt_timer.tick(*g_state, speech, now_ms);

        // Battery: sample the INA226 every few seconds and fan the result out to
        // the device UI (SharedState) + the BLE / Wi-Fi settings services.
        if (has_battery && now_ms >= next_battery_ms) {
            next_battery_ms = now_ms + kBatteryPeriodMs;
            if (auto r = battery.read()) {
                const int mv = static_cast<int>(r->voltage * 1000.0f + 0.5f);
                const int ma = static_cast<int>(r->current * 1000.0f + (r->current >= 0 ? 0.5f : -0.5f));
                const int pct = app::battery_percent_from_voltage(r->voltage);
                g_state->battery.mv.store(static_cast<std::int16_t>(mv), std::memory_order_relaxed);
                g_state->battery.ma.store(static_cast<std::int16_t>(ma), std::memory_order_relaxed);
                g_state->battery.pct.store(static_cast<std::int8_t>(pct), std::memory_order_relaxed);
                config::notify_battery(mv, ma, pct);
                wifi_config::set_battery(mv, ma, pct);
            }
        }

        // LCD touch (M5.Touch — the screen's capacitive touch, distinct from
        // the Si12T head sensor) drives the on-device UI. Forward every press
        // to the UI module, which hit-tests it against the current page.
        // Handled before the conversation/audio early-returns so the UI opens
        // in every mode.
        if (is_atom_nyan) {
            // AtomS3R: no LCD touch, single USER_BUT toggles the status overlay.
            app::atom_status::poll_button();
        } else {
            const auto td = M5.Touch.getDetail();
            // Horizontal flick → next/prev tab. M5Unified emits this on the
            // release frame after a touch that travelled past the flick
            // threshold. We treat it independently of the press path so a
            // press that ends in a flick doesn't also fire the tap action.
            if (td.wasFlicked()) {
                app::ui::handle_flick(td.distanceX(), td.distanceY());
            }
            if (td.wasPressed()) {
                // AP provisioning screen owns the touch while it's up —
                // the on-screen "終了" button is how the user dismisses
                // AP mode on touch boards (CoreS3 / StopWatch). A return
                // of true swallows the tap so it doesn't leak through to
                // the device_ui hit-test underneath.
                if (app::ap_screen::handle_tap(td.x, td.y)) {
                    continue;
                }
                const bool ui_consumed = app::ui::handle_tap(td.x, td.y);
                // A tap the on-device UI didn't consume (didn't open / use
                // the UI, didn't hit the mute corner), while the assistant is
                // mid-reply, is a barge-in request: voice input is paused for
                // the whole turn, so the screen tap is how the user
                // interrupts. The conversation task consumes this during
                // playback.
                if (!ui_consumed &&
                    g_state->barge_in_enabled.load(std::memory_order_relaxed) &&
                    g_state->conv.active.load(std::memory_order_relaxed) &&
                    !g_state->conv.idle.load(std::memory_order_relaxed)) {
                    g_state->barge_in_request.store(true, std::memory_order_relaxed);
                }
            }

            // StopWatch: while the user keeps a finger pressed in the
            // outer ring of the 466×466 round panel, bias the avatar's
            // gaze toward the touch point — the eyes follow the finger
            // as it slides around the rim. Center / device-UI region
            // taps are unaffected. saccade keeps running in parallel
            // (the VM sums saccade + gaze_target), so the eyes wander
            // naturally around the commanded direction rather than
            // locking dead-stop. Released → reset to (0, 0) and the
            // saccade-only behaviour resumes.
            if (touch_gaze_follow) {
                constexpr float kCenter = 233.0f;          // 466 / 2
                constexpr float kOuterR = 233.0f;          // panel edge
                constexpr float kInnerR = 180.0f;          // ring inner edge
                constexpr float kGazeGain = 5.0f;          // DSL multiplier
                                                            // is *3 → 15 px peak
                bool follow_active = false;
                if (td.isPressed() && !app::ui::active()) {
                    const float dx = static_cast<float>(td.x) - kCenter;
                    const float dy = static_cast<float>(td.y) - kCenter;
                    const float r  = std::sqrt(dx * dx + dy * dy);
                    if (r >= kInnerR && r <= kOuterR) {
                        // Normalise direction onto the unit circle, then
                        // amplify by kGazeGain so the offset is visible
                        // through the VM's gaze * 3 multiplier.
                        const float inv = 1.0f / r;
                        g_state->face.gaze_target_h.store(dx * inv * kGazeGain,
                                                     std::memory_order_relaxed);
                        g_state->face.gaze_target_v.store(dy * inv * kGazeGain,
                                                     std::memory_order_relaxed);
                        follow_active = true;
                    }
                }
                if (!follow_active) {
                    g_state->face.gaze_target_h.store(0.0f, std::memory_order_relaxed);
                    g_state->face.gaze_target_v.store(0.0f, std::memory_order_relaxed);
                }
            }
        }

        const bool conv_active = g_state->conv.active.load(std::memory_order_relaxed);
        const bool conv_idle = g_state->conv.idle.load(std::memory_order_relaxed);
        const bool audio_streaming = g_state->audio_stream_active.load(std::memory_order_relaxed);

        // While a BLE audio stream is playing, the streamer owns the speaker
        // and drives mouth_open itself. Stand down completely — stop any
        // in-flight babble (its playRaw would fight the stream on the I2S
        // bus) and don't touch mouth_open.
        if (audio_streaming) {
            if (speech.is_speaking()) speech.stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Idle behaviours (random head poses, nadenade) run when there is no
        // conversation OR the conversation is idly listening. The full demo
        // (mouth-sync, Wi-Fi balloon, babble, expression cycle) runs only when
        // there is no conversation at all — otherwise it would fight the
        // conversation task for the avatar and the I2S bus.
        const bool allow_idle_demo = !conv_active || conv_idle;
        const bool allow_full_demo = !conv_active;

        // While the conversation is thinking / speaking it owns the avatar —
        // stand down completely.
        if (!allow_idle_demo) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (allow_full_demo) {
            // When idle jtts babble is enabled, drive the mouth from the
            // speech envelope and run the Wi-Fi check + random babble. When
            // disabled, demo_loop becomes a no-op on the mouth so the mic
            // lip-sync task (main/mic_lip_sync_task.cpp), if active, owns
            // `mouth_open` without us overwriting it with 0 every tick.
            if (jtts_idle_enabled) {
                // Mouth opens with the current speech envelope; closed while silent.
                g_state->face.mouth_open.store(speech.current_mouth_open(), std::memory_order_relaxed);

                // The "Wi-Fi: 切断中" balloon and the babble suppression below only
                // make sense when the assistant actually needs the network — i.e.
                // when the conversation backend (OpenAI / Gemini / XiaoZhi) is on.
                // With conversation disabled the demo is fully self-contained
                // (local jtts babble), so we ignore Wi-Fi state entirely and let
                // the idle behaviour run from boot without waiting for an AP.
                const bool wifi_ok = !conversation_enabled || app::wifi_is_connected();
                if (!wifi_ok && !wifi_warning_active) {
                    speech.stop();
                    // hold_ms = UINT32_MAX so the balloon stays put until we clear it.
                    g_state->set_balloon_text("Wi-Fi: 切断中", /*hold_ms=*/UINT32_MAX);
                    balloon_in_flight.store(false, std::memory_order_release);
                    wifi_warning_active = true;
                } else if (wifi_ok && wifi_warning_active) {
                    g_state->clear_balloon();
                    wifi_warning_active = false;
                    next_speech_ms = now_ms + 1500;
                }

                // Kick off a new babble + balloon once the previous balloon is done
                // (callback resets balloon_in_flight) AND audio is idle AND the
                // random dwell time has elapsed. Suppressed while Wi-Fi is down so
                // the disconnected balloon stays visible.
                if (!wifi_warning_active &&
                    now_ms >= next_speech_ms &&
                    !speech.is_speaking() &&
                    !balloon_in_flight.load(std::memory_order_acquire)) {
                    // Speak a phrase and show ITS display text in the balloon —
                    // babble() returns the display (発話内容) of the same phrase
                    // it synthesises (発声内容), so screen and voice always match.
                    const std::string display = speech.babble(esp_random());
                    if (!display.empty()) {
                        balloon_in_flight.store(true, std::memory_order_release);
                        g_state->set_balloon_text(display, /*hold_ms=*/0, [] {
                            balloon_in_flight.store(false, std::memory_order_release);
                        });
                    }
                    next_speech_ms = now_ms + rand_range_ms(kSpeechMinMs, kSpeechMaxMs);
                }
            }
        }

        // Nadenade: poll the top sensor and look for a directional stroke
        // across the three zones. On a completed stroke, run a quick happy
        // head-wobble. The wobble blocks demo_loop's normal scheduling for
        // ~1.4 s but the render and servo tasks keep running.
        if (g_touch != nullptr && !wifi_warning_active && now_ms >= next_nadenade_ms) {
            const auto reading = g_touch->read();
            const std::uint8_t f = reading.front(), mid = reading.middle(), bk = reading.back();
            const std::uint8_t mx = std::max({f, mid, bk});

            // Edge-triggered diagnostic — only log when the reading
            // actually changes, otherwise a chip that gets stuck at
            // `2 2 2` from RFI floods the serial port at 20 Hz.
            static std::uint8_t last_logged[3] = {0xFF, 0xFF, 0xFF};
            if (f != last_logged[0] || mid != last_logged[1] || bk != last_logged[2]) {
                if (reading.any_touched() ||
                    last_logged[0] != 0 || last_logged[1] != 0 || last_logged[2] != 0) {
                    ESP_LOGI(kTag, "touch raw: front=%u middle=%u back=%u", f, mid, bk);
                }
                last_logged[0] = f;
                last_logged[1] = mid;
                last_logged[2] = bk;
            }

            // End (and clear) the gesture once the head's been all-quiet for
            // longer than the inter-zone gap.
            if (now_ms - stroke_active_ms > kStrokeGapMs) {
                stroke_hit_ms = {0, 0, 0};
            }
            if (mx > 0) stroke_active_ms = now_ms;

            // Record the first time each zone reaches a firm contact in this
            // gesture.
            if (f   >= kStrokePeakIntensity && stroke_hit_ms[0] == 0) stroke_hit_ms[0] = now_ms;
            if (mid >= kStrokePeakIntensity && stroke_hit_ms[1] == 0) stroke_hit_ms[1] = now_ms;
            if (bk  >= kStrokePeakIntensity && stroke_hit_ms[2] == 0) stroke_hit_ms[2] = now_ms;

            bool stroke_complete = false;
            if (stroke_hit_ms[0] && stroke_hit_ms[1] && stroke_hit_ms[2]) {
                // All three zones firmly touched within one gesture. Accept
                // only a monotonic onset order across the head, with the two
                // ends hit in different samples (so a single all-three RFI
                // spike — equal timestamps — can't qualify).
                const auto a = stroke_hit_ms[0], b = stroke_hit_ms[1], c = stroke_hit_ms[2];
                const bool fwd = a <= b && b <= c && a < c;   // front→middle→back
                const bool rev = a >= b && b >= c && a > c;   // back→middle→front
                stroke_complete = fwd || rev;
                if (!stroke_complete) {
                    // Hit all three but not cleanly ordered → drop so a noisy
                    // simultaneous lift can't linger and re-qualify.
                    stroke_hit_ms = {0, 0, 0};
                }
            }

            if (stroke_complete) {
                const char* direction =
                    stroke_hit_ms[0] < stroke_hit_ms[2] ? "front_to_back" : "back_to_front";
                ESP_LOGI(kTag, "nadenade! stroke %s (hit ms: f=%u m=%u b=%u)",
                         direction,
                         static_cast<unsigned>(stroke_hit_ms[0]),
                         static_cast<unsigned>(stroke_hit_ms[1]),
                         static_cast<unsigned>(stroke_hit_ms[2]));
                stackchan::wifi_config::mcp_events::publish_touch_stroke(direction);
                speech.stop();
                const float prev_yaw = g_state->servo.target_yaw_deg.load(std::memory_order_relaxed);
                const int prev_expr = g_state->face.expression.load(std::memory_order_relaxed);

                g_state->face.expression.store(static_cast<int>(avatar::Expression::Happy),
                                          std::memory_order_relaxed);
                g_state->servo.speed_override.store(800, std::memory_order_relaxed); // ~120°/s
                balloon_in_flight.store(true, std::memory_order_release);
                g_state->set_balloon_text("なでなで♡", /*hold_ms=*/2200, [] {
                    balloon_in_flight.store(false, std::memory_order_release);
                });

                constexpr float kWobbleDeg = 8.0f;
                constexpr std::uint32_t kHalfPeriodMs = 160;
                for (int i = 0; i < 4; ++i) {
                    g_state->servo.target_yaw_deg.store(-kWobbleDeg, std::memory_order_relaxed);
                    vTaskDelay(pdMS_TO_TICKS(kHalfPeriodMs));
                    g_state->servo.target_yaw_deg.store(+kWobbleDeg, std::memory_order_relaxed);
                    vTaskDelay(pdMS_TO_TICKS(kHalfPeriodMs));
                }
                g_state->servo.target_yaw_deg.store(prev_yaw, std::memory_order_relaxed);
                vTaskDelay(pdMS_TO_TICKS(kHalfPeriodMs));
                g_state->servo.speed_override.store(0, std::memory_order_relaxed);
                g_state->face.expression.store(prev_expr, std::memory_order_relaxed);

                stroke_hit_ms = {0, 0, 0};
                stroke_active_ms = 0;
                const std::uint32_t after_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
                next_nadenade_ms = after_ms + kNadenadeCooldownMs;
                // Push back demo activity so the wobble doesn't fight a
                // freshly-scheduled random pose / babble.
                next_speech_ms = after_ms + 1500;
                next_pose_ms = std::max(next_pose_ms, after_ms + 2000);
                continue;
            }
        }

        // Random yaw + pitch every 10–20 s.
        if (now_ms >= next_pose_ms) {
            g_state->servo.target_yaw_deg.store(rand_in(kYawMinDeg, kYawMaxDeg), std::memory_order_relaxed);
            g_state->servo.target_pitch_deg.store(rand_in(kPitchMinDeg, kPitchMaxDeg), std::memory_order_relaxed);
            next_pose_ms = now_ms + rand_range_ms(kPoseMinMs, kPoseMaxMs);
        }

        // Cycle expression every 5 s — full demo only; during a conversation
        // the model drives the expression via the set_expression tool.
        if (allow_full_demo && now_ms >= next_expression_ms) {
            g_state->face.expression.store(static_cast<int>(kCycle[expression_index]), std::memory_order_relaxed);
            expression_index = (expression_index + 1) % (sizeof(kCycle) / sizeof(kCycle[0]));
            next_expression_ms = now_ms + kExpressionPeriodMs;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace stackchan::app
