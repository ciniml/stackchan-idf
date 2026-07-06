// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "avatar/expression.hpp"

namespace stackchan::app {

// Coarse conversation-backend (OpenAI / Gemini Live) connection status for the
// on-device 会話 screen. Disabled is the default when the conversation task
// isn't running (feature off / no API key).
enum class ConvStatus : int {
    Disabled = 0,
    WaitingWifi,  // task up, waiting for Wi-Fi
    Connecting,   // opening the realtime session
    Listening,    // connected, idle listening
    Talking,      // in a turn (thinking / speaking)
    Yielded,      // handed off to BLE / Wi-Fi audio streaming
    Reconnecting, // recovering after a transport error
    Error,        // connect attempt failed
};

// A value guarded by a mutex with a lock-free version counter: the writer
// (BLE host / HTTP worker / boot seed) calls set(); a polling consumer
// (render task, demo_loop) watches version() each tick and takes a
// snapshot() only when it bumped, so the heavy copy happens off the writer's
// stack and only on actual change. This is the one pattern behind the face
// config / face bytecode / LT config / audio metrics slots (the balloon
// adds a completion callback and stays hand-rolled below).
template <typename T>
class VersionedValue {
public:
    void set(T value)
    {
        std::lock_guard lock{mutex_};
        value_ = std::move(value);
        version_.fetch_add(1, std::memory_order_release);
    }

    std::uint32_t version() const noexcept
    {
        return version_.load(std::memory_order_acquire);
    }

    T snapshot() const
    {
        std::lock_guard lock{mutex_};
        return value_;
    }

private:
    mutable std::mutex mutex_;
    T value_{};
    std::atomic<std::uint32_t> version_{0};
};

// State shared between the demo / servo / render / conversation / led tasks
// and the settings sinks. Grouped by domain; every leaf is either a
// lock-free atomic or a VersionedValue (mutex + version counter). See each
// group for ownership notes (who writes, who reads).
class SharedState {
public:
    using BalloonCompletionCallback = std::function<void()>;

    // --- Avatar face (render_task reads every frame) -----------------------
    struct Face {
        std::atomic<float> mouth_open{0.0f};
        std::atomic<int> expression{static_cast<int>(stackchan::avatar::Expression::Neutral)};
        // External gaze target (Avatar::set_gaze inputs). Updated by the
        // touch-driven gaze-follow path in demo_loop; read by render_task
        // every frame and forwarded to avatar.set_gaze. Animator-driven
        // saccade is added on top inside the avatar VM, so a non-zero
        // target biases the eyes toward (cos θ, sin θ) of the touch
        // direction while the eyes still wander naturally. Both default
        // 0 = no commanded gaze, animator runs alone.
        std::atomic<float> gaze_target_h{0.0f};
        std::atomic<float> gaze_target_v{0.0f};
    };
    Face face;

    // --- Servo (servo_task reads; demo_loop / conversation / UI write) -----
    struct Servo {
        std::atomic<float> target_yaw_deg{0.0f};
        std::atomic<float> target_pitch_deg{0.0f};
        // Non-zero overrides the servo task's default Goal Speed for the next
        // write_goal_position. Used for snappy gestures (head shake).
        std::atomic<std::uint16_t> speed_override{0};
        // Servo torque enable. The on-device 操作 (control) screen toggles
        // this; the servo task enables/disables torque to match (false =
        // head goes limp).
        std::atomic<bool> enabled{true};
        // Servo range-setting mode: forces torque off (head moves freely by
        // hand) and the servo task polls present-position via Read so the
        // settings UIs can capture zero / min / max while the user holds the
        // head at each pose. While set, the servo task ignores
        // target_yaw_deg / target_pitch_deg entirely. The captured raw steps
        // are published below.
        std::atomic<bool> range_mode{false};
        // Most recent present-position read in range mode (raw SCS step,
        // 0..1023). -1 = unread (servo absent / read failed since entering).
        std::atomic<std::int16_t> yaw_raw{-1};
        std::atomic<std::int16_t> pitch_raw{-1};
        // Servo motion mask: true while audible speech output is in
        // progress, so the servo task holds the head perfectly still (no
        // goal writes, no torque toggles). The servos and the speaker
        // amp/codec share a power rail, and a move's current draw sags it
        // enough to glitch / cut the audio. Set at speech START and cleared
        // at speech END by the application (the conversation task for
        // replies, demo_loop for idle babble) — NOT by polling the speaker,
        // whose isPlaying() briefly reads false in the gaps between streamed
        // reply segments and would let the head twitch mid-reply. (BLE /
        // Wi-Fi streaming is masked separately via audio_stream_active.)
        std::atomic<bool> masked{false};
    };
    Servo servo;

    // --- Conversation backend (conversation_task writes) -------------------
    struct Conversation {
        // True while a conversation session is live.
        std::atomic<bool> active{false};
        // True while the conversation is idly listening (not thinking /
        // speaking). demo_loop runs its idle behaviours (random head poses,
        // nadenade) when this is set, but keeps babble / expression-cycle /
        // mouth-sync to itself.
        std::atomic<bool> idle{false};
        // Connection status + a count of transport-error reconnects
        // (recover_after_error). The 会話 screen shows both so a reconnect
        // storm (repeated API connection failures) is visible.
        std::atomic<ConvStatus> status{ConvStatus::Disabled};
        std::atomic<std::uint32_t> reconnects{0};
        // Cooperative I2S handoff flag — see audio_stream_active below.
        std::atomic<bool> yielded_i2s{false};
    };
    Conversation conv;

    // --- Battery (demo_loop samples the INA226; UI + services read) --------
    struct Battery {
        // Snapshot refreshed periodically by demo_loop (the only task that
        // touches the internal I2C bus). The device UI reads these directly;
        // the BLE / Wi-Fi services receive their own pushed copies. mv / pct
        // stay at -1 until the first valid read (and if the INA226 is
        // absent), so all surfaces show "—".
        std::atomic<std::int16_t> mv{-1};  // bus voltage [mV]
        std::atomic<std::int16_t> ma{0};   // shunt current [mA] (discharge sign per wiring)
        std::atomic<std::int8_t> pct{-1};  // 0..100, or -1 = unknown
        // Whether the render task draws the top-left battery gauge over the
        // avatar. Seeded from NVS at boot (DeviceConfig::battery_gauge_enabled).
        std::atomic<bool> gauge_enabled{true};
    };
    Battery battery;

    // --- Speaker output (settings sinks + demo_loop watcher) ---------------
    struct Speaker {
        // User-set speaker output volume (integer percent 0..200, 100 =
        // factory default per board). Seeded from cfg.speaker_volume_pct at
        // boot; updated live by the BLE/HTTP/device UI sliders, which also
        // call M5.Speaker.setVolume() + save_speaker_volume() so the change
        // takes effect immediately and survives reboot.
        std::atomic<std::uint16_t> volume_pct{100};
        // One-touch speaker mute. Session-only (deliberately NOT persisted
        // to NVS — a reboot always restores sound). Toggled by the top-left
        // corner tap on touch boards (device_ui), the volume row's center
        // tap on the Control tab, or a 1.5 s BtnA hold-and-release on
        // AtomS3 / AtomS3R (atom_status). demo_loop watches this and
        // re-applies the effective M5.Speaker volume; render_task draws the
        // mute badge while set.
        std::atomic<bool> muted{false};
    };
    Speaker speaker;

    // --- Mic lip-sync calibration (mic_lip_sync_task reads every frame) ----
    struct MicLip {
        // Both as integer percent so sliders / BLE chr write u16 directly.
        // Input gain multiplies the mic RMS before normalisation (higher →
        // more sensitive); output gain scales the final 0..1 mouth-open
        // value (higher → wider mouth swings, clamped at 1.0). 100 = 1.0x.
        // The mic task reads these every iteration so changes take effect
        // on the next ~16 ms tick without a reboot.
        std::atomic<std::uint16_t> input_gain_pct{200};
        std::atomic<std::uint16_t> output_gain_pct{100};
        // Adaptive noise-floor AGC enable. When true (default), the mic
        // task rebases its dB window on the tracked ambient noise level;
        // when false, a fixed floor is used. Live — read every mic frame.
        std::atomic<bool> agc_enabled{true};
    };
    MicLip mic_lip;

    // --- Back-panel / nekomimi NeoPixel strip (led_task reads) -------------
    struct Led {
        // mode = 0: off, 1: solid, 2: breath (single-colour sine fade),
        //        3: gradient (rainbow cycle).
        // brightness: 0..255 master gain applied after mode → strip.
        // Boot in gradient (rainbow) mode so the strip cycles through
        // colours continuously — also keeps the I2C path well-exercised,
        // which is the ongoing soak test for the refresh_leds()
        // RMW-elimination fix (docs/known_issues.md §1).
        // Default brightness ~10 % (26/255). The PY32 base ring sat at
        // 96/255 (~37 %) which was fine through I²C; the GPIO9 nekomimi
        // chain has the diodes much closer to the user's eye so the same
        // gain reads as glare. Single u32 colour payload keeps the load
        // lock-free (0x00RRGGBB).
        std::atomic<std::uint8_t> mode{3}; // 3 = kModeGradient (rainbow)
        std::atomic<std::uint32_t> color{0x00404040u}; // ignored by gradient
        std::atomic<std::uint8_t> brightness{26};
        // Gradient (rainbow) revolution period in tenths of a second.
        // 60 = 6.0 s (the historical hardcoded value). led_task uses
        // (period_ds / 10) as the divisor on the hue ramp. Clamped to >= 1
        // on use so a stray zero doesn't divide-by-zero the animation.
        std::atomic<std::uint8_t> gradient_period_ds{60};
        // Modulate the LED brightness with `mouth_open` when on. Read by
        // main/led_task on every frame; live-applied from BLE/HTTP through
        // the staging path on Apply (reboot) — so this matches
        // `battery.gauge_enabled` semantics (NVS-persisted, not a slider).
        std::atomic<bool> mouth_sync_enabled{false};
        // Lip-sync renderer selection (config::LipSyncMode as u8:
        // 0=Brightness, 1=LevelMeter). Only consulted by main/led_task when
        // mouth_sync_enabled is true. Seeded from cfg.lip_sync_mode at
        // boot; live-writable via BLE/HTTP (no reboot needed).
        std::atomic<std::uint8_t> lip_sync_mode{0};
    };
    Led led;

    // --- LT timekeeper (main/lt_timer.cpp; ticked by demo_loop) ------------
    struct Lt {
        // The on-device LT tab writes command / total_s and renders active /
        // remaining_s. Command is exchange()d to 0 by the timer so each
        // press runs exactly once.
        std::atomic<std::uint8_t> command{0};    // 1 = start, 2 = stop/reset
        std::atomic<std::uint16_t> total_s{300}; // talk length (UI preset)
        std::atomic<bool> active{false};
        std::atomic<std::int32_t> remaining_s{300}; // negative = overtime
    };
    Lt lt;

    // --- Cross-cutting coordination flags (top-level by design) ------------

    // Cooperative quiesce for camera sessions. Two independent hazards:
    //
    // 1) I2C: the GC0308's SCCB shares the physical bus (G11/G12) AND the
    //    I2C controller with m5::In_I2C, but through a different software
    //    stack (IDF i2c_master vs lgfx's register-level driver). Any In_I2C
    //    access while esp_camera is initialised re-inits the controller
    //    under the SCCB driver's feet → SCCB_Write fails
    //    ESP_ERR_INVALID_STATE (the historical "camera vs touch/led race").
    //
    // 2) PSRAM bandwidth: the camera EDMA writes frames into a PSRAM fb.
    //    At RGB565 (2 B/px) the write rate collides with the render task's
    //    full-screen PSRAM sprite traffic (compose + push at 30 fps) and
    //    frames arrive torn — confirmed on hardware as bands of corrupt
    //    pixels interleaved with good rows.
    //
    // camera_service raises this flag for the duration of its session (the
    // ONLY writer); the periodic In_I2C users (demo_loop's M5.update /
    // battery / nadenade / IMU poll, led_task's strip refresh) AND the
    // render task skip their work while it's set. Sessions are one-shot and
    // ~1.5 s, so the pause (frozen face, LEDs holding state) is invisible
    // in normal use.
    std::atomic<bool> i2c_quiesce{false};

    // Cooperative I2S handoff for BLE audio streaming. CoreS3 shares the
    // I2S_NUM_1 bus between mic + speaker, so audio_stream_sink can't just
    // grab the speaker while the conv-task is mid-listening (mic_task would
    // race the M5.Mic.end teardown and stack-overflow). Instead:
    //   1. audio_stream_sink sets audio_stream_active = true,
    //   2. conv-task observes, ends mic + speaker, sets conv.yielded_i2s = true,
    //   3. audio_stream_sink begins the speaker + plays,
    //   4. audio_stream_sink ends the speaker + clears audio_stream_active,
    //   5. conv-task sees the flag clear and re-enters Listening.
    std::atomic<bool> audio_stream_active{false};

    // Touch barge-in master switch. Read on every touch-dispatch tick in
    // demo_loop; when false the screen-tap path that sets
    // `barge_in_request` is skipped entirely. Seeded from
    // DeviceConfig::barge_in_enabled at boot.
    std::atomic<bool> barge_in_enabled{false};

    // Set by the LCD touch handler (demo_loop) when the screen is tapped
    // while the assistant is responding and the tap wasn't consumed by the
    // on-device UI. The conversation task consumes it during playback to
    // barge in (stop the reply, return to listening) — the intended way to
    // interrupt now that voice input is paused for the whole assistant turn.
    std::atomic<bool> barge_in_request{false};

    // --- Balloon (mutex + completion callback; render_task consumes) -------

    // Show `text` in the balloon.
    //  - hold_ms: minimum on-screen time (0 = use avatar defaults — short
    //    text holds a few seconds, long text plays one marquee pass).
    //  - on_complete: invoked once when the balloon finishes (after hold or
    //    after a marquee pass). Fired from the render task; the
    //    implementation must be cheap and thread-safe.
    void set_balloon_text(std::string_view text,
                          std::uint32_t hold_ms = 0,
                          BalloonCompletionCallback on_complete = {})
    {
        std::lock_guard lock{balloon_mutex_};
        balloon_text_.assign(text);
        balloon_hold_ms_ = hold_ms;
        balloon_callback_ = std::move(on_complete);
        balloon_version_.fetch_add(1, std::memory_order_release);
        balloon_visible_.store(true, std::memory_order_release);
    }

    // Force-clear the balloon. Completion callback is dropped without firing.
    void clear_balloon()
    {
        std::lock_guard lock{balloon_mutex_};
        balloon_text_.clear();
        balloon_hold_ms_ = 0;
        balloon_callback_ = nullptr;
        balloon_version_.fetch_add(1, std::memory_order_release);
        balloon_visible_.store(false, std::memory_order_release);
    }

    // Called by the render task when the avatar finishes displaying the
    // current balloon. Hides the balloon and invokes the completion callback
    // (if any) outside the lock.
    void notify_balloon_complete()
    {
        BalloonCompletionCallback cb;
        {
            std::lock_guard lock{balloon_mutex_};
            if (!balloon_visible_.load(std::memory_order_relaxed)) {
                return; // already cleared
            }
            balloon_text_.clear();
            balloon_hold_ms_ = 0;
            cb = std::move(balloon_callback_);
            balloon_callback_ = nullptr;
            balloon_version_.fetch_add(1, std::memory_order_release);
            balloon_visible_.store(false, std::memory_order_release);
        }
        if (cb) {
            cb();
        }
    }

    // Returns the current balloon version (incremented on every change).
    std::uint32_t balloon_version() const noexcept
    {
        return balloon_version_.load(std::memory_order_acquire);
    }

    bool balloon_visible() const noexcept
    {
        return balloon_visible_.load(std::memory_order_acquire);
    }

    // Copies the current text + hold time into the supplied outputs.
    void snapshot_balloon(std::string& text_out, std::uint32_t& hold_ms_out) const
    {
        std::lock_guard lock{balloon_mutex_};
        text_out = balloon_text_;
        hold_ms_out = balloon_hold_ms_;
    }

    // --- Versioned slots (VersionedValue facade — see the template above) --

    // Live avatar face tuning (eye/eyebrow/mouth geometry + colours), carried
    // as the compact JSON the settings UI sends over BLE. The render task
    // parses it (off the BLE host task) and applies it via
    // Avatar::set_face_tuning when the version bumps. Set at boot from NVS
    // and live on every BLE write.
    void set_face_config(std::string_view json) { face_config_.set(std::string{json}); }
    std::uint32_t face_config_version() const noexcept { return face_config_.version(); }
    std::string snapshot_face_config() const { return face_config_.snapshot(); }

    // LT timekeeper config (warn/over announcement words + thresholds),
    // carried as compact JSON. BLE/HTTP write it from their host tasks;
    // demo_loop polls the version and re-runs LtTimer::configure off the
    // writer's stack.
    void set_lt_config(std::string_view json) { lt_config_.set(std::string{json}); }
    std::uint32_t lt_config_version() const noexcept { return lt_config_.version(); }
    std::string snapshot_lt_config() const { return lt_config_.snapshot(); }

    // Live avatar DSL bytecode (the .avbc binary the avatar_vm runs). Set at
    // boot from NVS (empty if none persisted), and replaced live via the
    // BLE / Wi-Fi upload sinks. The render task polls the version, and on a
    // bump calls Avatar::load_face_bytecode (or reset_face_bytecode when
    // empty).
    void set_face_bytecode(std::span<const std::uint8_t> bytes)
    {
        face_bytecode_.set({bytes.begin(), bytes.end()});
    }
    void clear_face_bytecode() { face_bytecode_.set({}); }
    std::uint32_t face_bytecode_version() const noexcept { return face_bytecode_.version(); }
    std::vector<std::uint8_t> snapshot_face_bytecode() const { return face_bytecode_.snapshot(); }

    // Audio pipeline metrics (one snapshot per finished/aborted reply).
    // Conv-task fills this at end-of-turn in log_playback_metrics(); HTTP
    // (`GET /api/metrics/audio`) and BLE (chr 0x1f) read it back.
    struct AudioMetrics {
        std::uint32_t turn_at_ms = 0;          // now_ms() when the snapshot was written
        std::uint32_t chunk_count = 0;
        std::uint32_t speaker_sample_rate = 0;  // nominal Hz (24000 for Gemini, 8000 for OpenAI µ-law)
        // Event-queue hop latency: emit_us → conv-task receive
        float recv_lag_us_avg = 0, recv_lag_us_min = 0, recv_lag_us_max = 0;
        // Recv → handed-to-playRaw delay
        float recv_to_queued_ms_avg = 0, recv_to_queued_ms_min = 0, recv_to_queued_ms_max = 0;
        // M5.Speaker.isPlaying queue depth (0..2)
        float spk_queue_avg = 0, spk_queue_min = 0, spk_queue_max = 0;
        // Unplayed samples in assistant_pcm_ at sample time
        float pcm_lag_samples_avg = 0, pcm_lag_samples_max = 0;
        // Effective sps inferred from seg_pos_ / elapsed; compare against
        // speaker_sample_rate to detect I2S clock drift.
        float played_sps = 0;
        // Mic-uplink chunks evicted (oldest-dropped) from the backend
        // client's TX queue, cumulative since session start. Non-zero means
        // the WebSocket uplink couldn't keep up with mic capture.
        std::uint32_t tx_evicted_chunks = 0;
    };
    void update_audio_metrics(const AudioMetrics& m) { audio_metrics_.set(m); }
    std::uint32_t audio_metrics_version() const noexcept { return audio_metrics_.version(); }
    AudioMetrics snapshot_audio_metrics() const { return audio_metrics_.snapshot(); }

private:
    mutable std::mutex balloon_mutex_;
    std::string balloon_text_;
    std::uint32_t balloon_hold_ms_{0};
    BalloonCompletionCallback balloon_callback_{};
    std::atomic<std::uint32_t> balloon_version_{0};
    std::atomic<bool> balloon_visible_{false};

    VersionedValue<std::string> face_config_;
    VersionedValue<std::string> lt_config_;
    VersionedValue<std::vector<std::uint8_t>> face_bytecode_;
    VersionedValue<AudioMetrics> audio_metrics_;
};

} // namespace stackchan::app
