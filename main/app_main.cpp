// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <M5Unified.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include <nvs_flash.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>

#include "atom_status.hpp"
#if CONFIG_STACKCHAN_AUDIO_STREAM_ENABLED
#include "audio_stream_sink.hpp"
#endif
#include "avatar/expression.hpp"
#include "avatar_vm/storage.hpp"
#include "battery.hpp"
#include "board/audio_module_es8388.hpp"
#include "board/board.hpp"
#include "board/si12t_touch.hpp"
#include "config_service/config_service.hpp"
#include "config_service/config_store.hpp"
#include <wifi_config_service/mcp_events.hpp>
#include <wifi_config_service/wifi_config_service.hpp>
#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
#include "conversation_task.hpp"
#endif
#include "device_ui.hpp"
#include "diag.hpp"
#include "i2c_dump.hpp"
#include "led_task.hpp"
#include "lt_timer.hpp"
#include "qr_task.hpp"
#include "render_task.hpp"
#include "servo_limits.hpp"
#include "servo_task.hpp"
#include "shared_state.hpp"
#include "speech.hpp"
#include "utf8.hpp"
#if CONFIG_STACKCHAN_WIFI_AUDIO_ENABLED
#include "wifi_audio.hpp"
#endif
#include "wifi_sta.hpp"

#include <jtts/jtts.hpp>
#ifdef CONFIG_TELEGRAM_PHASE1_ENABLED
#include "telegram/telegram.hpp"
#endif

namespace {

constexpr const char* kTag = "stackchan";

// Debug-only switch: when true, the NeoPixel strip stays uninitialised AND the
// led_task is never spawned. Kept around because the lgfx i2c mutex has a
// long-running race (xTaskPriorityDisinherit assert at led_task_entry →
// refresh_leds → readRegister8 → i2c_wait → unlock → xQueueGenericSend) — see
// docs/known_issues.md §1. Flip to true to disable the task while
// investigating; refresh_leds() has since been switched to a single-write
// path (no RMW) to halve I2C activity, which should reduce the race rate.
constexpr bool kLedTaskDisabledForDebug = false;

// Heap-allocate so the task argument outlives app_main's scope (the tasks run forever).
stackchan::app::SharedState* g_state = nullptr;
stackchan::app::RenderTaskArgs* g_render_args = nullptr;
stackchan::app::ServoTaskArgs* g_servo_args = nullptr;
#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
stackchan::app::ConversationTaskArgs* g_conversation_args = nullptr;
#endif
stackchan::app::LedTaskArgs* g_led_args = nullptr;
stackchan::board::Si12tTouch* g_touch = nullptr;

// Live face-config sink: invoked from the BLE host task on each FaceConfig
// WRITE. Just stashes the raw JSON in SharedState (cheap, host-task-safe); the
// render task parses + applies it. config_service guarantees g_state is set
// before BLE comes online (see boot order in app_main).
void on_face_config(std::string_view json)
{
    if (g_state != nullptr) {
        g_state->set_face_config(json);
    }
}

// Range-mode sink + live-positions getter shared by BLE and Wi-Fi services.
// Sink mutates SharedState; the servo task picks up the flag on its next
// iteration and disables/enables torque accordingly.
void on_servo_range_mode(bool on)
{
    if (g_state != nullptr) {
        g_state->servo_range_mode.store(on, std::memory_order_relaxed);
    }
}
stackchan::config::ServoPositionsView servo_positions()
{
    if (g_state == nullptr) return {-1, -1};
    return {g_state->servo_yaw_raw.load(std::memory_order_relaxed),
            g_state->servo_pitch_raw.load(std::memory_order_relaxed)};
}

// LED state pulled live out of SharedState atomics. Used by both BLE chr 0x20
// READ and HTTP `GET /api/led-state`.
stackchan::config::LedState read_led_state()
{
    stackchan::config::LedState s{};
    if (g_state == nullptr) return s;
    const std::uint32_t color = g_state->led_color.load(std::memory_order_relaxed);
    s.mode = g_state->led_mode.load(std::memory_order_relaxed);
    s.r = static_cast<std::uint8_t>((color >> 16) & 0xFF);
    s.g = static_cast<std::uint8_t>((color >>  8) & 0xFF);
    s.b = static_cast<std::uint8_t>( color        & 0xFF);
    s.brightness = g_state->led_brightness.load(std::memory_order_relaxed);
    s.gradient_period_ds = g_state->led_gradient_period_ds.load(std::memory_order_relaxed);
    return s;
}

// Apply a patch onto SharedState. led_color packs the three components into a
// single u32 so the load above stays lock-free; we read-modify-write here
// since at most one writer (BLE host task or HTTP worker) is touching it.
void apply_led_patch(const stackchan::config::LedStatePatch& p)
{
    if (g_state == nullptr) return;
    if (p.mode) {
        const std::uint8_t m = *p.mode;
        // Clamp invalid modes to "off" rather than ignoring — easier to debug
        // a typo from a client than a silently-dropped value.
        g_state->led_mode.store(m <= 3 ? m : 0, std::memory_order_relaxed);
    }
    if (p.r || p.g || p.b) {
        std::uint32_t cur = g_state->led_color.load(std::memory_order_relaxed);
        std::uint8_t cr = static_cast<std::uint8_t>((cur >> 16) & 0xFF);
        std::uint8_t cg = static_cast<std::uint8_t>((cur >>  8) & 0xFF);
        std::uint8_t cb = static_cast<std::uint8_t>( cur        & 0xFF);
        if (p.r) cr = *p.r;
        if (p.g) cg = *p.g;
        if (p.b) cb = *p.b;
        g_state->led_color.store((static_cast<std::uint32_t>(cr) << 16) |
                                 (static_cast<std::uint32_t>(cg) <<  8) |
                                 static_cast<std::uint32_t>(cb),
                                 std::memory_order_relaxed);
    }
    if (p.brightness) {
        g_state->led_brightness.store(*p.brightness, std::memory_order_relaxed);
    }
    if (p.gradient_period_ds) {
        // Clamp 0 → 1 so the divisor in led_task never hits zero. The wire
        // protocol accepts the full u8 range; we just refuse the one
        // pathological value here rather than sprinkle clamps on every read.
        const std::uint8_t v = *p.gradient_period_ds == 0 ? 1 : *p.gradient_period_ds;
        g_state->led_gradient_period_ds.store(v, std::memory_order_relaxed);
    }
    // Persist immediately so a reboot replays the same look. The settings
    // UI debounces writes ~150 ms so we don't write to NVS faster than the
    // user can drag a slider; HTTP clients posting in a loop are on their
    // own (no debounce here).
    const std::uint8_t mode = g_state->led_mode.load(std::memory_order_relaxed);
    const std::uint32_t color = g_state->led_color.load(std::memory_order_relaxed);
    const std::uint8_t bright = g_state->led_brightness.load(std::memory_order_relaxed);
    const std::uint8_t period_ds = g_state->led_gradient_period_ds.load(std::memory_order_relaxed);
    (void)stackchan::config::store::save_led_state(mode, color, bright, period_ds);
}

// Render the last-turn audio metrics as JSON for BLE chr 0x1f + HTTP
// `GET /api/metrics/audio`. Same getter is wired to both transports so
// clients see the same payload regardless of how they connect. Returns
// "{}" before the first turn finishes.
std::string audio_metrics_json()
{
    if (g_state == nullptr) return "{}";
    const auto m = g_state->snapshot_audio_metrics();
    if (m.turn_at_ms == 0) return "{}";
    char buf[640];
    std::snprintf(
        buf, sizeof(buf),
        "{"
        "\"turn_at_ms\":%u,"
        "\"chunks\":%u,"
        "\"speaker_sample_rate\":%u,"
        "\"played_sps\":%.1f,"
        "\"recv_lag_us\":{\"avg\":%.0f,\"min\":%.0f,\"max\":%.0f},"
        "\"recv_to_queued_ms\":{\"avg\":%.1f,\"min\":%.1f,\"max\":%.1f},"
        "\"spk_queue\":{\"avg\":%.2f,\"min\":%.0f,\"max\":%.0f},"
        "\"pcm_lag_samples\":{\"avg\":%.0f,\"max\":%.0f}"
        "}",
        static_cast<unsigned>(m.turn_at_ms),
        static_cast<unsigned>(m.chunk_count),
        static_cast<unsigned>(m.speaker_sample_rate),
        static_cast<double>(m.played_sps),
        static_cast<double>(m.recv_lag_us_avg),
        static_cast<double>(m.recv_lag_us_min),
        static_cast<double>(m.recv_lag_us_max),
        static_cast<double>(m.recv_to_queued_ms_avg),
        static_cast<double>(m.recv_to_queued_ms_min),
        static_cast<double>(m.recv_to_queued_ms_max),
        static_cast<double>(m.spk_queue_avg),
        static_cast<double>(m.spk_queue_min),
        static_cast<double>(m.spk_queue_max),
        static_cast<double>(m.pcm_lag_samples_avg),
        static_cast<double>(m.pcm_lag_samples_max));
    return std::string{buf};
}

// Live-apply an avatar face bytecode upload that just landed via HTTP
// (`POST /api/avatar-dsl`) or BLE chr 0x21 (commit op). Both transports
// persist to NVS before calling this — we only update the SharedState slot
// that the render task polls (data=nullptr / len=0 means "revert to the
// firmware-embedded default"). Returns true unconditionally for now; the
// SharedState slot can't fail. Captured as a function pointer so it can be
// passed to both `config::set_avatar_bytecode_sink` (BLE) and
// `wifi_config::set_avatar_bytecode_sink` (HTTP, accepts std::function).
bool apply_avatar_bytecode(const std::uint8_t* data, std::size_t len)
{
    if (g_state == nullptr) return false;
    if (data == nullptr || len == 0) {
        g_state->clear_face_bytecode();
    } else {
        g_state->set_face_bytecode({data, len});
    }
    return true;
}

// CoreS3 mic + speaker share I2S_NUM_1, so we have to hand the bus around
// explicitly. Records `seconds` of audio at 16 kHz then plays it straight
// back. Blocks the caller for ~2 * seconds.
void record_and_playback(std::uint32_t seconds, const char* label)
{
    constexpr std::uint32_t kSampleRate = 16'000;
    std::vector<std::int16_t> buf(kSampleRate * seconds, 0);

    while (M5.Speaker.isPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    M5.Speaker.end();
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(kTag, "%s: recording %u s...", label, static_cast<unsigned>(seconds));
    if (!M5.Mic.record(buf.data(), buf.size(), kSampleRate, /*stereo=*/false)) {
        ESP_LOGE(kTag, "M5.Mic.record returned false");
        return;
    }
    for (int i = 0; i < 50 && M5.Mic.isRecording() == 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    M5.Mic.end();
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(kTag, "%s: playing back...", label);
    M5.Speaker.playRaw(buf.data(), buf.size(), kSampleRate, /*stereo=*/false);
    while (M5.Speaker.isPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(kTag, "%s: done", label);
}

void demo_loop(const std::string& jtts_config_json, bool has_battery, bool is_atom_nyan, bool conversation_enabled, const stackchan::app::ServoLimits& limits)
{
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

    // Set true by the (render-task) completion callback so demo_loop knows
    // the previous balloon finished. Atomics keep it thread-safe.
    static std::atomic<bool> balloon_in_flight{false};

    // Wi-Fi state edge detection: while disconnected we pin a persistent
    // "Wi-Fi: 切断中" balloon and suppress babble; when it reconnects we
    // clear the balloon so normal demo behaviour resumes.
    bool wifi_warning_active = false;

    for (;;) {
        // Drive M5.update() so M5.Touch / M5.BtnPWR latch their state machines.
        M5.update();

        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);

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
                g_state->battery_mv.store(static_cast<std::int16_t>(mv), std::memory_order_relaxed);
                g_state->battery_ma.store(static_cast<std::int16_t>(ma), std::memory_order_relaxed);
                g_state->battery_pct.store(static_cast<std::int8_t>(pct), std::memory_order_relaxed);
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
            if (td.wasPressed()) {
                const bool ui_before = app::ui::active();
                app::ui::handle_tap(td.x, td.y);
                // A tap that didn't open/use the on-device UI, while the
                // assistant is mid-reply, is a barge-in request: voice input is
                // paused for the whole turn, so the screen tap is how the user
                // interrupts. The conversation task consumes this during
                // playback.
                if (!ui_before && !app::ui::active() &&
                    g_state->conversation_active.load(std::memory_order_relaxed) &&
                    !g_state->conversation_idle.load(std::memory_order_relaxed)) {
                    g_state->barge_in_request.store(true, std::memory_order_relaxed);
                }
            }
        }

        const bool conv_active = g_state->conversation_active.load(std::memory_order_relaxed);
        const bool conv_idle = g_state->conversation_idle.load(std::memory_order_relaxed);
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
            // Mouth opens with the current speech envelope; closed while silent.
            g_state->mouth_open.store(speech.current_mouth_open(), std::memory_order_relaxed);

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
                const float prev_yaw = g_state->target_yaw_deg.load(std::memory_order_relaxed);
                const int prev_expr = g_state->expression.load(std::memory_order_relaxed);

                g_state->expression.store(static_cast<int>(avatar::Expression::Happy),
                                          std::memory_order_relaxed);
                g_state->servo_speed_override.store(800, std::memory_order_relaxed); // ~120°/s
                balloon_in_flight.store(true, std::memory_order_release);
                g_state->set_balloon_text("なでなで♡", /*hold_ms=*/2200, [] {
                    balloon_in_flight.store(false, std::memory_order_release);
                });

                constexpr float kWobbleDeg = 8.0f;
                constexpr std::uint32_t kHalfPeriodMs = 160;
                for (int i = 0; i < 4; ++i) {
                    g_state->target_yaw_deg.store(-kWobbleDeg, std::memory_order_relaxed);
                    vTaskDelay(pdMS_TO_TICKS(kHalfPeriodMs));
                    g_state->target_yaw_deg.store(+kWobbleDeg, std::memory_order_relaxed);
                    vTaskDelay(pdMS_TO_TICKS(kHalfPeriodMs));
                }
                g_state->target_yaw_deg.store(prev_yaw, std::memory_order_relaxed);
                vTaskDelay(pdMS_TO_TICKS(kHalfPeriodMs));
                g_state->servo_speed_override.store(0, std::memory_order_relaxed);
                g_state->expression.store(prev_expr, std::memory_order_relaxed);

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
            g_state->target_yaw_deg.store(rand_in(kYawMinDeg, kYawMaxDeg), std::memory_order_relaxed);
            g_state->target_pitch_deg.store(rand_in(kPitchMinDeg, kPitchMaxDeg), std::memory_order_relaxed);
            next_pose_ms = now_ms + rand_range_ms(kPoseMinMs, kPoseMaxMs);
        }

        // Cycle expression every 5 s — full demo only; during a conversation
        // the model drives the expression via the set_expression tool.
        if (allow_full_demo && now_ms >= next_expression_ms) {
            g_state->expression.store(static_cast<int>(kCycle[expression_index]), std::memory_order_relaxed);
            expression_index = (expression_index + 1) % (sizeof(kCycle) / sizeof(kCycle[0]));
            next_expression_ms = now_ms + kExpressionPeriodMs;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace

// Temporary heap monitor — periodic snapshot of internal/PSRAM free + the
// largest contiguous block. Useful while chasing slow leaks or fragmentation
// that surface as mbedtls handshake failures ("esp-aes: Failed to allocate
// memory"). Cheap to run; remove once the audio-tx refactor is settled.
[[maybe_unused]] static void heap_monitor_task(void* /*arg*/)
{
    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const std::size_t int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const std::size_t int_big   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const std::size_t dma_big   = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        const std::size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const std::size_t psram_big  = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        const std::size_t int_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        const std::size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        // DMA-largest is the gating metric for the esp-aes hardware path (TLS
        // record bounce buffers) — INT-largest alone overstates what TLS can
        // actually grab. See conversation_task.cpp's recover-wait gate.
        ESP_LOGI("heap",
                 "INT free=%u (largest=%u, min=%u)  DMA largest=%u  PSRAM free=%u (largest=%u, min=%u)",
                 static_cast<unsigned>(int_free), static_cast<unsigned>(int_big), static_cast<unsigned>(int_min),
                 static_cast<unsigned>(dma_big),
                 static_cast<unsigned>(psram_free), static_cast<unsigned>(psram_big), static_cast<unsigned>(psram_min));
        // Every 60 s: per-task stack high-water marks, so stack budgets can be
        // right-sized from measurement (guessed reductions broke boot twice).
        if (++tick % 6 == 0) {
            stackchan::app::diag_stack_hwm();
        }
    }
}

extern "C" void app_main()
{
    // Surface why we (re)booted. After an unexpected reboot this pins the cause
    // — ESP_RST_BROWNOUT (power sag, e.g. sustained speaker/servo current),
    // ESP_RST_PANIC (crash/abort/assert), ESP_RST_INT_WDT / ESP_RST_TASK_WDT
    // (a task hogged the CPU). Note: opening the serial port with the repo's
    // monitor_log.py pulses DTR/RTS and shows up here as a USB/external reset,
    // so use `make monitor` (stays attached) to catch a *natural* crash reason.
    ESP_LOGW(kTag, "reset reason: %d", static_cast<int>(esp_reset_reason()));

    // Log every failed heap alloc (size + caps + caller + remaining heap).
    // This is how the esp-aes "Failed to allocate memory" finally gets a
    // number attached — register before anything else can fail.
    stackchan::app::diag_register_alloc_fail_hook();

    // Confirm the running image so the bootloader doesn't roll back on the
    // next reboot. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y leaves freshly-OTA'd
    // images in PENDING_VERIFY until this call promotes them to VALID. We do
    // it unconditionally — for boot from the original factory partition it's
    // a no-op, for boot after an OTA it locks in the new firmware.
    esp_ota_mark_app_valid_cancel_rollback();

    xTaskCreatePinnedToCore(heap_monitor_task, "heap_mon", 3072, nullptr, 1, nullptr, 1);

    auto board_result = stackchan::board::Board::begin();
    if (!board_result) {
        ESP_LOGE(kTag, "Board::begin() failed: %d", static_cast<int>(board_result.error()));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    auto& board = *board_result;

    // Reserve the conversation task's internal-RAM speaker ring *now*, before
    // anything else has a chance to chew up DRAM. The conversation task itself
    // doesn't start until Wi-Fi associates (~12 s in), and by then other
    // subsystems (camera link's IRAM overhead, mbedtls sessions, BLE pools,
    // SSE monitor stack, …) have driven the largest contiguous internal-RAM
    // block below the 8 KiB we need per segment, and the alloc fails. Doing it
    // here (right after Board::begin(), largest ≈ 29 KiB) is the only stable
    // window. Ownership lives here — conv-task only reads from these buffers
    // and never frees them. nullptr on failure → conv-task disables itself
    // cleanly, the rest of the firmware keeps running.
#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
    std::array<std::int16_t*, stackchan::app::kConversationSegmentBuffers> g_conv_seg_buf{};
    {
        bool all_ok = true;
        for (auto& buf : g_conv_seg_buf) {
            buf = static_cast<std::int16_t*>(heap_caps_malloc(
                stackchan::app::kConversationSegmentSamples * sizeof(std::int16_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (buf == nullptr) {
                all_ok = false;
            }
        }
        if (all_ok) {
            const std::size_t largest =
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            ESP_LOGI(kTag,
                     "seg_buf reserved early: %ux%u B internal (largest now=%u B)",
                     static_cast<unsigned>(stackchan::app::kConversationSegmentBuffers),
                     static_cast<unsigned>(stackchan::app::kConversationSegmentSamples *
                                           sizeof(std::int16_t)),
                     static_cast<unsigned>(largest));
        } else {
            ESP_LOGE(kTag,
                     "failed to reserve seg_buf at boot — conversation will be disabled");
        }
    }
#endif // CONFIG_STACKCHAN_CONVERSATION_ENABLED

    // Diagnostic register dump of the internal-I2C chips (AXP2101 / AW9523 /
    // PY32). Read-only; needed for debugging the recurring "LCD backlight off
    // after LED init" issue — the dump captures whatever state the chips
    // landed in at boot so we can diff against a healthy boot. Runs BEFORE
    // any LED / PY32 access in the rest of app_main so the snapshot reflects
    // the post-corruption state we want to investigate, not a fresh-write
    // state we created ourselves.
    stackchan::app::dump_internal_i2c_registers();

    // CoreS3 Speaker and Mic share I2S_NUM_1 (BCK=GPIO34, WS=GPIO33),
    // so the side that's done has to release the bus before the other
    // side can install its own driver.

    // M5Unified's mic/speaker I2S tasks default to priority 2 — below the
    // render (5), conversation (5), servo (4) and WebSocket (5) tasks — so
    // they get starved and the I2S DMA underruns: choppy playback and gappy
    // capture (which whisper then mistranscribes). Lift them above the app
    // tasks and give the speaker extra DMA buffering for jitter margin.
    // Module Audio (M144, ES8388 codec): probe once at boot. With its
    // jumpers in Config B the codec already listens on the same I2S1 bus as
    // the internal AW88298 (BCK=34, WS=33, DIN=13); we just have to start
    // emitting MCLK on GPIO0 (the AW88298 never needed it) and program the
    // codec over I2C. Both outputs then play in parallel: internal 1 W
    // speaker + the module's line/headphone jacks (→ active speaker for
    // venue-level volume, e.g. the LT timekeeper announcements).
    const bool has_audio_module = stackchan::board::es8388::probe();
    if (has_audio_module) {
        if (auto r = stackchan::board::es8388::init(); r) {
            ESP_LOGI(kTag, "Module Audio (ES8388) detected — line-out enabled");
        } else {
            ESP_LOGW(kTag, "Module Audio (ES8388) detected but init failed");
        }
    }

    {
        auto spk = M5.Speaker.config();
        spk.task_priority = 6;
        spk.dma_buf_count = 16;
        // Pin to core 1. Default (tskNO_AFFINITY) lets the speaker task
        // land on core 0 where NimBLE + Wi-Fi live; at priority 6 it
        // out-prioritises NimBLE's host task and steals CPU during
        // audio playback, which drops BLE RX throughput from ~22 KiB/s
        // to ~10 KiB/s and turns BLE audio streaming choppy.
        spk.task_pinned_core = 1;
        M5.Speaker.config(spk);
        M5.Speaker.end();

        auto mic = M5.Mic.config();
        mic.task_priority = 6;
        mic.task_pinned_core = 1;
        M5.Mic.config(mic);
        M5.Mic.end();
    }

    // Quick audio sanity check: a short rising arpeggio so we can hear
    // immediately whether the speaker is wired up correctly.
    M5.Speaker.setVolume(128);
    for (float freq : {523.25f, 659.25f, 783.99f}) { // C5 – E5 – G5
        M5.Speaker.tone(freq, 150);
        vTaskDelay(pdMS_TO_TICKS(180));
    }
    while (M5.Speaker.isPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    M5.Speaker.end();
    vTaskDelay(pdMS_TO_TICKS(20));

    // NVS must be initialised exactly once, before NimBLE host and Wi-Fi.
    {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
    }

    static stackchan::config::DeviceConfig cfg = stackchan::config::load();

    // SharedState + audio_stream sink must be live BEFORE config::start
    // brings the BLE GATT service online. Otherwise a client that
    // connects early (well within the 5–10 s of Wi-Fi / mic / servo
    // bring-up that follows) sends `begin` to an unregistered sink and
    // the entire audio session is silently dropped — every subsequent
    // audio_data write sees g_audio_sink == nullptr and bails.
    g_state = new stackchan::app::SharedState{};
    // Seed the avatar face tuning from NVS (empty → built-in default face) and
    // register the live BLE update sink, both before config::start brings the
    // GATT service online so an early client write is applied immediately.
    g_state->set_face_config(cfg.face_config_json);
    // LT timekeeper config: seeding bumps the version, so demo_loop's poll
    // applies it on its first iteration (no special boot path needed).
    if (!cfg.lt_config_json.empty()) {
        g_state->set_lt_config(cfg.lt_config_json);
    }
    g_state->battery_gauge_enabled.store(cfg.battery_gauge_enabled, std::memory_order_relaxed);
    // LED state: replay the persisted values so the strip lights up the same
    // way it did before the reboot. NVS-missing → DeviceConfig's struct
    // defaults (gradient @ ~10%) so a fresh-install device still glows.
    g_state->led_mode.store(cfg.led_mode, std::memory_order_relaxed);
    g_state->led_color.store(cfg.led_color, std::memory_order_relaxed);
    g_state->led_brightness.store(cfg.led_brightness, std::memory_order_relaxed);
    g_state->led_gradient_period_ds.store(
        cfg.led_gradient_period_ds == 0 ? 1 : cfg.led_gradient_period_ds,
        std::memory_order_relaxed);
    stackchan::config::set_face_config_sink(&on_face_config);
    stackchan::config::set_lt_config_sink(+[](std::string_view json) {
        if (g_state != nullptr) g_state->set_lt_config(json);
    });
    stackchan::config::set_servo_range_mode_sink(&on_servo_range_mode);
    stackchan::config::set_servo_positions_getter(&servo_positions);
    stackchan::config::set_audio_metrics_getter(&audio_metrics_json);
    stackchan::config::set_led_state_getter(&read_led_state);
    stackchan::config::set_led_state_sink(&apply_led_patch);
    // Tell the settings services which board we're on so the web UIs can hide
    // sections that don't apply (e.g. servo config on Atom-nyan). Must happen
    // before config::start / wifi_config setup so the first central read sees
    // the right value.
    stackchan::config::set_board_kind(static_cast<std::uint8_t>(board.kind()));
    // BLE audio streaming and the realtime voice conversation are mutually
    // exclusive — both saturate the radio/CPU and running them together
    // makes streaming playback choppy. Pass the conversation-enabled flag
    // so the sink refuses `begin` while voice chat is on.
#if CONFIG_STACKCHAN_AUDIO_STREAM_ENABLED
    stackchan::app::audio_stream::start(*g_state, cfg.openai_enabled);
#else
    ESP_LOGI(kTag, "audio_stream: disabled at compile time (slim build)");
#endif

    if (auto r = stackchan::config::start(cfg); !r) {
        ESP_LOGE(kTag, "BLE config service failed to start: %d (continuing without BLE)",
                 static_cast<int>(r.error()));
    }

    stackchan::app::wifi_start(cfg);
    // Same sink/getter on the Wi-Fi side. The Wi-Fi service starts on a worker
    // task after Wi-Fi STA gets an IP — the calls below race that; the setters
    // tolerate being called before the HTTP server is up (the values are
    // cached in static storage and applied once the handlers register).
    stackchan::wifi_config::set_servo_range_mode_sink(&on_servo_range_mode);
    stackchan::wifi_config::set_servo_positions_getter(&servo_positions);
    stackchan::wifi_config::set_audio_metrics_getter(&audio_metrics_json);
    stackchan::wifi_config::set_led_state_getter(&read_led_state);
    stackchan::wifi_config::set_led_state_sink(&apply_led_patch);
    stackchan::wifi_config::set_board_kind(static_cast<std::uint8_t>(board.kind()));
    // (Channel /mcp/events bring-up happens AFTER start_conversation_task so
    //  the conv-task gets first dibs on contiguous internal RAM for its 3 ×
    //  8 KB segment buffers + TLS handshake. See below.)

#ifdef CONFIG_TELEGRAM_PHASE1_ENABLED
    // Phase 1 throwaway: once Wi-Fi STA has an IP, fire ONE getUpdates request
    // and log the result. Confirms HTTPS to api.telegram.org + token validity
    // + JSON parse before we build the full polling loop in Phase 2. Token
    // comes from sdkconfig.defaults.local (gitignored). 10 KiB stack — the
    // request itself uses ~6 KiB for TLS + mbedtls scratch.
    xTaskCreatePinnedToCore(
        +[](void* /*arg*/) {
            // Wait for STA connection. wifi_audio's reader does the same dance.
            while (!stackchan::app::wifi_is_connected()) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            constexpr const char* token = CONFIG_TELEGRAM_PHASE1_BOT_TOKEN;
            if (token[0] == '\0') {
                ESP_LOGW(kTag, "telegram: CONFIG_TELEGRAM_PHASE1_BOT_TOKEN is empty, skipping probe");
                vTaskDelete(nullptr);
                return;
            }
            auto r = stackchan::telegram::get_updates_one_shot(token, /*offset=*/0, /*timeout_sec=*/5);
            if (r) {
                ESP_LOGI(kTag, "telegram phase1 probe OK (max_update_id=%lld)",
                         static_cast<long long>(*r));
            } else {
                ESP_LOGE(kTag, "telegram phase1 probe FAILED: %s",
                         stackchan::telegram::to_string(r.error()));
            }
            vTaskDelete(nullptr);
        },
        "tg_probe", 10240, nullptr, tskIDLE_PRIORITY + 2, nullptr, 1);
#endif // CONFIG_TELEGRAM_PHASE1_ENABLED

    // Avatar face DSL bytecode: restore any user-uploaded override from NVS
    // (no-op when the slot is empty), then register the upload sink so
    // POST /api/avatar-dsl can hot-swap the bytecode live. The render task
    // polls SharedState::face_bytecode_version() and feeds the bytes into
    // Avatar::load_face_bytecode (or reset_face_bytecode() when empty).
    if (auto loaded = stackchan::avatar_vm::storage::load(); loaded) {
        ESP_LOGI(kTag, "avatar_vm: restored %u bytes of face bytecode from NVS",
                 static_cast<unsigned>(loaded->size()));
        g_state->set_face_bytecode(*loaded);
    } else if (loaded.error() != stackchan::avatar_vm::storage::StorageError::NotFound) {
        ESP_LOGW(kTag, "avatar_vm: load failed (%s) — using firmware default",
                 stackchan::avatar_vm::storage::to_string(loaded.error()));
    }
    // Same closure for both transports: chr 0x21 (BLE) and POST /api/avatar-dsl
    // (HTTP). The free function decays to a function pointer for the BLE
    // setter, and converts implicitly to std::function for the HTTP one.
    stackchan::config::set_avatar_bytecode_sink(&apply_avatar_bytecode);
    stackchan::wifi_config::set_avatar_bytecode_sink(&apply_avatar_bytecode);

    // Channel adapter (/mcp/*) sinks. Expression / balloon ride the existing
    // SharedState pipelines (render_task picks them up next frame). `say`
    // spawns a one-shot worker because synthesis + playback can take
    // hundreds of ms, way too long to block the HTTP server task.
    stackchan::wifi_config::set_mcp_expression_sink(
        [](std::string_view name) {
            if (g_state == nullptr) return;
            // Map enum names to the Expression integer. Unknown names fall
            // back to Neutral rather than rejecting — the HTTP handler has
            // already accepted the request, so silent fallback is the kinder
            // failure mode.
            using E = stackchan::avatar::Expression;
            int v = static_cast<int>(E::Neutral);
            if (name == "happy")        v = static_cast<int>(E::Happy);
            else if (name == "sad")     v = static_cast<int>(E::Sad);
            else if (name == "angry")   v = static_cast<int>(E::Angry);
            else if (name == "doubt")   v = static_cast<int>(E::Doubt);
            else if (name == "sleepy")  v = static_cast<int>(E::Sleepy);
            else if (name == "neutral") v = static_cast<int>(E::Neutral);
            g_state->expression.store(v, std::memory_order_relaxed);
        });

    stackchan::wifi_config::set_mcp_balloon_sink(
        [](std::string_view text, std::uint32_t hold_ms) {
            if (g_state == nullptr) return;
            g_state->set_balloon_text(text, hold_ms);
        });

    stackchan::wifi_config::set_lt_config_sink([](std::string_view json) {
        if (g_state != nullptr) g_state->set_lt_config(json);
    });

    stackchan::wifi_config::set_mcp_say_kana_sink(
        [](std::string_view kana_utf8) {
            // Heap-copy the bytes — `kana_utf8` is owned by the HTTP request
            // and won't survive the handler return. The task frees it.
            auto* owned = new std::string{kana_utf8};
            // 12 KiB stack in PSRAM (not internal RAM): steady-state internal-
            // RAM largest is ~10 KiB after conversation_task brings up TLS, so
            // an internal-RAM 12 KiB stack alloc silently fails (the firmware
            // returns {"ok":true} but the worker is never scheduled, and the
            // owned string leaks). The worker only touches PSRAM-friendly
            // surfaces — jtts working buffers, PCM vector, M5.Speaker
            // playRaw enqueue — none of which disable cache, so PSRAM stack
            // is safe; vTaskDeleteWithCaps frees the stack at self-delete.
            constexpr UBaseType_t kCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
            const BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
                +[](void* arg) {
                    std::unique_ptr<std::string> kana_text{static_cast<std::string*>(arg)};
                    std::u32string kana = stackchan::app::decode_utf8(*kana_text);
                    if (kana.empty()) {
                        ESP_LOGW(kTag, "/mcp/say: empty / invalid utf8");
                        vTaskDeleteWithCaps(nullptr);
                        return;
                    }
                    constexpr std::uint32_t kRate = 16000;
                    stackchan::jtts::Options opt;
                    opt.voice = stackchan::jtts::Voice::Female;
                    opt.sample_rate_hz = kRate;
                    std::vector<std::int16_t> pcm;
                    if (auto r = stackchan::jtts::synthesize(kana, pcm, opt); !r) {
                        ESP_LOGW(kTag, "/mcp/say synth fail: %s",
                                 stackchan::jtts::to_string(r.error()));
                        vTaskDeleteWithCaps(nullptr);
                        return;
                    }
                    if (pcm.empty()) {
                        vTaskDeleteWithCaps(nullptr);
                        return;
                    }
                    // Wait for any in-flight audio to finish so we don't
                    // splice mid-utterance. playRaw queues internally so it
                    // returns quickly, but we still wait here on the worker
                    // to avoid two /mcp/say calls clobbering each other.
                    while (M5.Speaker.isPlaying()) vTaskDelay(pdMS_TO_TICKS(20));
                    M5.Speaker.playRaw(pcm.data(), pcm.size(), kRate, /*stereo=*/false);
                    // Wait for the speaker queue to drain so the say_done event
                    // matches the audible completion point (Claude can chain
                    // "wait until done, then ...").
                    while (M5.Speaker.isPlaying()) vTaskDelay(pdMS_TO_TICKS(20));
                    stackchan::wifi_config::mcp_events::publish_say_done();
                    vTaskDeleteWithCaps(nullptr);
                },
                // Pin to CPU 0 (not 1). CPU 1 hosts speaker/mic (prio 6),
                // render (5), servo (4) — adding a 12 KiB PSRAM-stack worker
                // there starves render long enough that touch taps get lost
                // and IDLE1 trips task_wdt (observed 2026-06-07). CPU 0 has
                // httpd + NimBLE event tasks but much more headroom.
                "mcp_say", 12 * 1024, owned, tskIDLE_PRIORITY + 2, nullptr, 0, kCaps);
            if (rc != pdPASS) {
                // Without this log the firmware silently swallows the request
                // (handler already returned 200 OK to the adapter). Surface
                // the failure mode + the PSRAM headroom to diagnose later.
                ESP_LOGE(kTag, "/mcp/say worker task create FAILED (PSRAM largest=%u)",
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
                delete owned;
            }
        });

    // Wi-Fi live audio (RTP/L16 today). Like the BLE sink, mutually exclusive
    // with the conversation backend, so it self-disables when voice chat is on.
#if CONFIG_STACKCHAN_WIFI_AUDIO_ENABLED
    stackchan::app::wifi_audio::start(*g_state, cfg.openai_enabled, cfg.rtp_audio_enabled);
#else
    ESP_LOGI(kTag, "wifi_audio: disabled at compile time (slim build)");
#endif

    // Mic / loopback sanity check at startup.
    record_and_playback(2, "mic test");

    // Treat AtomS3 (slim) and AtomNyan (full AtomS3R) the same for the
    // "no servo / no battery / minimal status overlay" decisions below —
    // both have no servo bus, no INA226, no Si12T head touch. The conv /
    // audio / wifi-audio stack differences are handled by Kconfig gates
    // (CONFIG_STACKCHAN_*_ENABLED) further down, not by BoardKind.
    const bool is_atom_nyan =
        board.kind() == stackchan::board::BoardKind::AtomNyan ||
        board.kind() == stackchan::board::BoardKind::AtomS3;

    // Servo bring-up is only meaningful on boards that actually have a servo
    // bus (CoreS3 + M5/Takao). Atom-nyan has no servos in Phase 1 scope; skip
    // both the power-rail enable and the 1.5 s settle wait. Also gated by the
    // NVS-persisted master switch cfg.servo_enabled (settable from BLE / Wi-Fi
    // / on-device UI — distinct from SharedState::servo_enabled which is the
    // live torque toggle).
    if (!is_atom_nyan && cfg.servo_enabled) {
        if (auto r = board.set_servo_power(true); !r) {
            ESP_LOGE(kTag, "set_servo_power(true) failed: %d", static_cast<int>(r.error()));
        }
        // Allow the servo bus rail to settle before the servo task starts
        // driving UART. SCS0009 needs ~1 s after Vmotor comes up before it
        // answers PING.
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else if (!cfg.servo_enabled) {
        ESP_LOGW(kTag, "servo VM rail OFF: cfg.servo_enabled=false (set via settings UI)");
    }

    g_render_args = new stackchan::app::RenderTaskArgs{.display = &board.display(), .state = g_state};
    const auto servo_limits = stackchan::app::parse_servo_limits(cfg.servo_limits_json);
    if (!is_atom_nyan) {
        const auto sb = board.servo_bus_config();
        g_servo_args = new stackchan::app::ServoTaskArgs{
            .state = g_state,
            .bus_cfg = {.uart = sb.uart, .tx = sb.tx, .rx = sb.rx, .baud = sb.baud,
                        .timeout_ms = 20, .echo_cancel = sb.echo_cancel},
            .limits = servo_limits,
        };
    }
    g_touch = board.touch_sensor();

    // API key + provider: pick whichever backend the user configured. The
    // openai_enabled flag still acts as a master "conversation off" switch
    // regardless of provider; turning it off keeps both keys in NVS.
    const char* api_key = "";
    const char* xiaozhi_url = "";
    const char* xiaozhi_token = "";
    if (!cfg.openai_enabled) {
        ESP_LOGI(kTag, "Conversation disabled by configuration");
    } else if (cfg.provider == stackchan::config::Provider::Gemini) {
        if (!cfg.gemini_api_key.empty()) {
            api_key = cfg.gemini_api_key.c_str();
        }
        ESP_LOGI(kTag, "provider=Gemini Live, key=%s",
                 api_key[0] ? "set" : "empty");
    } else if (cfg.provider == stackchan::config::Provider::XiaoZhi) {
        // XiaoZhi is keyed by its server URL; the token is optional.
        xiaozhi_url = cfg.xiaozhi_url.c_str();
        xiaozhi_token = cfg.xiaozhi_token.c_str();
        ESP_LOGI(kTag, "provider=XiaoZhi, url=%s token=%s",
                 xiaozhi_url[0] ? "set" : "empty", xiaozhi_token[0] ? "set" : "empty");
    } else {
        if (!cfg.openai_api_key.empty()) {
            api_key = cfg.openai_api_key.c_str();
        } else {
            api_key = CONFIG_STACKCHAN_OPENAI_API_KEY;
        }
        ESP_LOGI(kTag, "provider=OpenAI Realtime, key=%s",
                 api_key[0] ? "set" : "empty");
    }

#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
    g_conversation_args = new stackchan::app::ConversationTaskArgs{
        .state = g_state, .api_key = api_key, .provider = cfg.provider, .touch = g_touch,
        .xiaozhi_url = xiaozhi_url, .xiaozhi_token = xiaozhi_token,
        .system_prompt = cfg.system_prompt.c_str(),
        .extra_headers = cfg.conv_extra_headers.c_str(),
        .seg_buf = g_conv_seg_buf};
#else
    (void)api_key; (void)xiaozhi_url; (void)xiaozhi_token;
#endif

    // On-device UI is per-board: CoreS3 gets the 5-tab touchscreen settings UI,
    // Atom-nyan gets the minimal status overlay toggled by USER_BUT. Only one
    // is initialised; render_task dispatches based on which one's active().
    if (is_atom_nyan) {
        stackchan::app::atom_status::init(*g_state);
    } else {
        stackchan::app::ui::init(*g_state);
    }
    stackchan::app::start_render_task(*g_render_args);
    if (!is_atom_nyan && cfg.servo_enabled && g_servo_args != nullptr) {
        stackchan::app::start_servo_task(*g_servo_args);
    }
    // NeoPixel animation task. Driven by SharedState (led_mode / led_color /
    // led_brightness). Only spun up when the board actually has a strip
    // (CoreS3 = GPIO9, AtomNyan = GPIO38; both surface a NekomimiLedStrip).
    if (auto* strip = board.led_strip(); strip != nullptr && !kLedTaskDisabledForDebug) {
        g_led_args = new stackchan::app::LedTaskArgs{g_state, strip};
        stackchan::app::start_led_task(*g_led_args);
    } else if (kLedTaskDisabledForDebug) {
        ESP_LOGW(kTag, "led_task intentionally NOT started (kLedTaskDisabledForDebug)");
    }
    // The conversation task waits for Wi-Fi internally, then takes over the
    // I2S bus for always-on voice chat. Started after the boot-time mic test
    // so the two never contend for the bus. Gated by Kconfig so the AtomS3
    // (no-PSRAM) slim profile drops the whole TLS / WebSocket / assistant
    // PCM ring stack at compile time.
#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
    stackchan::app::start_conversation_task(*g_conversation_args);
#else
    ESP_LOGI(kTag, "conversation: disabled at compile time (slim build)");
#endif

    // Channel /mcp/events bring-up — deferred until after conv-task is created
    // so the conv-task's seg_buf_ alloc (3 × 8 KB internal-RAM contiguous) and
    // initial TLS / WebSocket setup don't race the SSE queue + monitor-task
    // stack for the same pool. mcp_events::start allocates the queue (in
    // PSRAM) and a 3 KiB internal-RAM stack for the diff monitor — small but
    // fragmenting if it lands before the segment buffers.
    stackchan::wifi_config::mcp_events::start(+[]() -> int {
        return g_state == nullptr ? 0
                                  : static_cast<int>(g_state->conversation_status.load(
                                        std::memory_order_relaxed));
    });
    // Fire the boot event once Wi-Fi has an IP — Claude wants the address +
    // FW version up front, both meaningless before the IP is assigned.
    xTaskCreatePinnedToCore(
        +[](void* arg) {
            const std::uint8_t kind = *static_cast<std::uint8_t*>(arg);
            delete static_cast<std::uint8_t*>(arg);
            while (!stackchan::app::wifi_is_connected()) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            const auto* desc = esp_app_get_description();
            char ip[16] = "-";
            esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t info{};
            if (nif != nullptr && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr != 0) {
                std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
            }
            stackchan::wifi_config::mcp_events::publish_boot(
                desc ? desc->version : "?", ip, kind);
            vTaskDelete(nullptr);
        },
        // 3 KiB minimum — esp_netif_* lookups + the heap_caps_xxx that
        // ESP_LOGI on the publish path may exercise tip a 2 KiB stack into
        // overflow (observed at boot on 2026-06-07).
        "mcp_boot", 3072, new std::uint8_t{static_cast<std::uint8_t>(board.kind())},
        tskIDLE_PRIORITY + 1, nullptr, 0);

    ESP_LOGI(kTag, "ready");

#if CONFIG_STACKCHAN_QR_TEST_AT_BOOT
    // Phase 1+2 bring-up trigger: spawn a one-shot waiter that defers the
    // QR scanner spin-up until 30 s after `ready`, giving Wi-Fi STA, BLE
    // advertising, mic test, conversation task, etc. time to settle so
    // their internal-RAM allocations don't race the camera DMA descriptors.
    // Replaced by a device_ui (Phase 3) start/stop button — remove this
    // block when that lands.
    xTaskCreatePinnedToCore(
        +[](void* arg) {
            auto* b = static_cast<stackchan::board::Board*>(arg);
            vTaskDelay(pdMS_TO_TICKS(30000));
            ESP_LOGI(kTag, "QR test: starting scanner (boot+30s)");
            (void)stackchan::app::start_qr_scan(*b);
            vTaskDelete(nullptr);
        },
        "qr_boot", 3072, &board, tskIDLE_PRIORITY + 1, nullptr, 0);
#endif

    demo_loop(cfg.jtts_config_json, board.has_battery(), is_atom_nyan,
              cfg.openai_enabled, servo_limits);
}
