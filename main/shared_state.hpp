// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

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

// State shared between the demo / servo / render tasks. Most fields are
// lock-free atomics; the balloon text + completion callback need a mutex
// because std::string and std::function aren't trivially copyable.
class SharedState {
public:
    using BalloonCompletionCallback = std::function<void()>;

    std::atomic<float> target_yaw_deg{0.0f};
    std::atomic<float> target_pitch_deg{0.0f};
    std::atomic<float> mouth_open{0.0f};
    std::atomic<int> expression{static_cast<int>(avatar::Expression::Neutral)};
    // Non-zero overrides the servo task's default Goal Speed for the next
    // write_goal_position. Used for snappy gestures (head shake).
    std::atomic<std::uint16_t> servo_speed_override{0};

    // True while a conversation session is live (set by the conversation task).
    std::atomic<bool> conversation_active{false};
    // True while the conversation is idly listening (not thinking / speaking).
    // demo_loop runs its idle behaviours (random head poses, nadenade) when
    // this is set, but keeps babble / expression-cycle / mouth-sync to itself.
    std::atomic<bool> conversation_idle{false};

    // Conversation backend connection status + a count of transport-error
    // reconnects (recover_after_error). The 会話 screen shows both so a
    // reconnect storm (repeated API connection failures) is visible.
    std::atomic<ConvStatus> conversation_status{ConvStatus::Disabled};
    std::atomic<std::uint32_t> conversation_reconnects{0};

    // Cooperative I2S handoff for BLE audio streaming. CoreS3 shares the
    // I2S_NUM_1 bus between mic + speaker, so audio_stream_sink can't just
    // grab the speaker while the conv-task is mid-listening (mic_task would
    // race the M5.Mic.end teardown and stack-overflow). Instead:
    //   1. audio_stream_sink sets audio_stream_active = true,
    //   2. conv-task observes, ends mic + speaker, sets conversation_yielded_i2s = true,
    //   3. audio_stream_sink begins the speaker + plays,
    //   4. audio_stream_sink ends the speaker + clears audio_stream_active,
    //   5. conv-task sees the flag clear and re-enters Listening.
    std::atomic<bool> audio_stream_active{false};
    std::atomic<bool> conversation_yielded_i2s{false};

    // Servo torque enable. The on-device 操作 (control) screen toggles this;
    // the servo task enables/disables torque to match (false = head goes limp).
    std::atomic<bool> servo_enabled{true};

    // Servo motion mask: true while audible speech output is in progress, so
    // the servo task holds the head perfectly still (no goal writes, no torque
    // toggles). The servos and the speaker amp/codec share a power rail, and a
    // move's current draw sags it enough to glitch / cut the audio. Set at
    // speech START and cleared at speech END by the application (the
    // conversation task for replies, demo_loop for idle babble) — NOT by
    // polling the speaker, whose isPlaying() briefly reads false in the gaps
    // between streamed reply segments and would let the head twitch mid-reply.
    // (BLE / Wi-Fi streaming is masked separately via audio_stream_active.)
    std::atomic<bool> servo_masked{false};

    // Set by the LCD touch handler (demo_loop) when the screen is tapped while
    // the assistant is responding and the tap wasn't consumed by the on-device
    // UI. The conversation task consumes it during playback to barge in (stop
    // the reply, return to listening) — the intended way to interrupt now that
    // voice input is paused for the whole assistant turn.
    std::atomic<bool> barge_in_request{false};

    // Show `text` in the balloon.
    //  - hold_ms: minimum on-screen time (0 = use avatar defaults — short
    //    text holds a few seconds, long text plays one marquee pass).
    //  - on_complete: invoked once when the balloon finishes (after hold or
    //    after a marquee pass). Fired from the render task; the implementation
    //    must be cheap and thread-safe.
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

    // Live avatar face tuning (eye/eyebrow/mouth geometry + colours), carried
    // as the compact JSON the settings UI sends over BLE. Stored as a raw
    // string + version counter; the render task parses it (off the BLE host
    // task) and applies it via Avatar::set_face_tuning when the version bumps.
    // Set at boot from NVS and live on every BLE write.
    void set_face_config(std::string_view json)
    {
        std::lock_guard lock{face_config_mutex_};
        face_config_json_.assign(json);
        face_config_version_.fetch_add(1, std::memory_order_release);
    }

    std::uint32_t face_config_version() const noexcept
    {
        return face_config_version_.load(std::memory_order_acquire);
    }

    std::string snapshot_face_config() const
    {
        std::lock_guard lock{face_config_mutex_};
        return face_config_json_;
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

private:
    mutable std::mutex balloon_mutex_;
    std::string balloon_text_;
    std::uint32_t balloon_hold_ms_{0};
    BalloonCompletionCallback balloon_callback_{};
    std::atomic<std::uint32_t> balloon_version_{0};
    std::atomic<bool> balloon_visible_{false};

    mutable std::mutex face_config_mutex_;
    std::string face_config_json_;
    std::atomic<std::uint32_t> face_config_version_{0};
};

} // namespace stackchan::app
