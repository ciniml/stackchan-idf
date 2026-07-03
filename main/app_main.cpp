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

#include "ap_screen.hpp"
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
#if CONFIG_STACKCHAN_CAMERA_ENABLED
#include "board/camera_gc0308.hpp"
#endif
#include "led_task.hpp"
#include "lt_timer.hpp"
#include "mic_lip_sync_task.hpp"
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
// Global Board accessor — only used from demo_loop for things that don't
// fit into the existing args (e.g. Board::vibrate() haptic pulses on
// StopWatch). Set once after Board::begin() before any task starts.
stackchan::board::Board* g_board = nullptr;

// Per-board factory speaker volume (the "100 %" reference for the user
// gain). Captured once at boot; the live-apply sink multiplies by the
// user's percent and clamps to 255. Default 128 = M5Unified's CoreS3
// half-scale until the boot path overwrites it.
std::uint8_t g_speaker_base_volume = 128;

// jtts::Options used by start_say_worker (and therefore by both the
// /api/jtts-say settings test button and BLE chr 0x2d). Populated at
// boot from cfg.jtts_config_json so the settings page's "話す" button
// uses the user's chosen voice / pitch / mora / formant rather than the
// hard-coded female-child preset. Same options the demo_loop babble
// uses (both go through resolve_speech_options on the same JSON).
stackchan::jtts::Options g_say_opts{};
bool g_say_opts_ready = false;

// Live-apply the user's speaker_volume_pct (0..200) to the M5.Speaker
// master volume and mirror into SharedState. Called from boot and from
// the BLE / HTTP / device-UI sinks so the effect is immediate. No NVS
// write here — the caller handles persistence (save_speaker_volume).
void apply_speaker_volume(std::uint16_t pct) noexcept
{
    if (pct > 200) pct = 200;
    int v = static_cast<int>(g_speaker_base_volume) * static_cast<int>(pct) / 100;
    if (v > 255) v = 255;
    if (v < 0) v = 0;
    M5.Speaker.setVolume(static_cast<std::uint8_t>(v));
    if (g_state != nullptr) {
        g_state->speaker_volume_pct.store(pct, std::memory_order_relaxed);
    }
}

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

// Mic lip-sync calibration — read live atomic values for BLE chr 0x23 / HTTP
// GET /api/mic-lip-gain. Falls back to 100 (= 1.0x) on any 0 read so a stray
// uninitialised slot can't drive the mic task into divide-by-zero.
stackchan::config::MicLipGain read_mic_lip_gain()
{
    stackchan::config::MicLipGain g{100, 100};
    if (g_state == nullptr) return g;
    const std::uint16_t in_pct = g_state->mic_lip_input_gain_pct.load(std::memory_order_relaxed);
    const std::uint16_t out_pct = g_state->mic_lip_output_gain_pct.load(std::memory_order_relaxed);
    g.input_pct = in_pct ? in_pct : 100;
    g.output_pct = out_pct ? out_pct : 100;
    return g;
}

// Mic lip-sync calibration sink. Clamps to 10..1000 % to keep the math sane,
// writes the atomics for live apply, then persists via single-writer
// save_mic_lip_gain so the values survive reboot.
void apply_mic_lip_gain(const stackchan::config::MicLipGain& g)
{
    if (g_state == nullptr) return;
    auto clamp = [](std::uint16_t v) -> std::uint16_t {
        if (v < 10) return 10;
        if (v > 1000) return 1000;
        return v;
    };
    const std::uint16_t in_pct  = clamp(g.input_pct);
    const std::uint16_t out_pct = clamp(g.output_pct);
    g_state->mic_lip_input_gain_pct.store(in_pct, std::memory_order_relaxed);
    g_state->mic_lip_output_gain_pct.store(out_pct, std::memory_order_relaxed);
    (void)stackchan::config::store::save_mic_lip_gain(in_pct, out_pct);
}

std::uint16_t read_speaker_volume_pct()
{
    if (g_state == nullptr) return 100;
    return g_state->speaker_volume_pct.load(std::memory_order_relaxed);
}

void apply_speaker_volume_sink(std::uint16_t pct)
{
    if (pct > 200) pct = 200;
    apply_speaker_volume(pct);  // sets M5.Speaker.setVolume + SharedState
    (void)stackchan::config::store::save_speaker_volume(pct);
}

// Spawn a PSRAM-stack worker that synthesises `kana_utf8` via jtts and
// pushes it through M5.Speaker.playRaw. Shared by /mcp/say (external MCP
// gate) and the settings-page test-speak buttons (BLE chr + /api/jtts-say,
// HTTP-auth gate). Returns immediately; the heap-owned string is freed
// either by the worker or on task-create failure here.
void start_say_worker(std::string_view kana_utf8)
{
    auto* owned = new std::string{kana_utf8};
    // 12 KiB stack in PSRAM. See the long rationale on the original
    // /mcp/say wiring (steady-state internal-RAM largest is ~10 KiB after
    // conversation_task TLS, so an internal-RAM 12 KiB stack alloc would
    // silently fail). The worker only touches PSRAM-friendly surfaces
    // (jtts buffers, PCM vector, M5.Speaker.playRaw enqueue).
    constexpr UBaseType_t kCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        +[](void* arg) {
            std::unique_ptr<std::string> kana_text{static_cast<std::string*>(arg)};
            std::u32string kana = stackchan::app::decode_utf8(*kana_text);
            if (kana.empty()) {
                ESP_LOGW(kTag, "say: empty / invalid utf8");
                vTaskDeleteWithCaps(nullptr);
                return;
            }
            // Use the user's jtts settings (voice / pitch / mora /
            // formant / vibrato) cached at boot. Falls back to the
            // default-options preset when no JSON has been saved yet
            // (g_say_opts_ready stays false until app_main sets it).
            stackchan::jtts::Options opt = g_say_opts_ready
                ? g_say_opts
                : stackchan::app::resolve_speech_options("", stackchan::app::Speech::kSampleRate);
            const std::uint32_t rate = opt.sample_rate_hz;
            std::vector<std::int16_t> pcm;
            if (auto r = stackchan::jtts::synthesize(kana, pcm, opt); !r) {
                ESP_LOGW(kTag, "say synth fail: %s",
                         stackchan::jtts::to_string(r.error()));
                vTaskDeleteWithCaps(nullptr);
                return;
            }
            if (pcm.empty()) {
                vTaskDeleteWithCaps(nullptr);
                return;
            }
            while (M5.Speaker.isPlaying()) vTaskDelay(pdMS_TO_TICKS(20));
            M5.Speaker.playRaw(pcm.data(), pcm.size(), rate, /*stereo=*/false);
            while (M5.Speaker.isPlaying()) vTaskDelay(pdMS_TO_TICKS(20));
            stackchan::wifi_config::mcp_events::publish_say_done();
            vTaskDeleteWithCaps(nullptr);
        },
        // Pin to CPU 0 — CPU 1 hosts speaker/mic/render/servo and a
        // 12 KiB stack alloc here starves render (observed 2026-06-07).
        "say_worker", 12 * 1024, owned, tskIDLE_PRIORITY + 2, nullptr, 0, kCaps);
    if (rc != pdPASS) {
        ESP_LOGE(kTag, "say worker task create FAILED (PSRAM largest=%u)",
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        delete owned;
    }
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

void demo_loop(const std::string& jtts_config_json, bool has_battery, bool is_atom_nyan, bool conversation_enabled, bool jtts_idle_enabled, const stackchan::app::ServoLimits& limits)
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

    // Last-applied speaker volume percent. Watches the SharedState atom
    // so the device-UI's −/+ nudge buttons re-apply via the same
    // setVolume + NVS path the BLE / WiFi sinks use. Seeded from the
    // current atom (set by boot's apply_speaker_volume_sink call).
    std::uint16_t last_speaker_volume_pct =
        g_state->speaker_volume_pct.load(std::memory_order_relaxed);

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
        // Drive M5.update() so M5.Touch / M5.BtnPWR latch their state machines.
        M5.update();

        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);

        // Live-apply speaker_volume_pct changes from the device-UI (BLE /
        // HTTP sinks already apply directly). Cheap atomic compare per
        // iteration; only triggers M5.Speaker.setVolume + NVS write on
        // an actual change.
        {
            const std::uint16_t cur = g_state->speaker_volume_pct.load(std::memory_order_relaxed);
            if (cur != last_speaker_volume_pct) {
                last_speaker_volume_pct = cur;
                apply_speaker_volume_sink(cur);
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
                    const int cur = g_state->expression.load(std::memory_order_relaxed);
                    int next = cur;
                    for (int i = 0; i < 4 && next == cur; ++i) {
                        next = static_cast<int>(esp_random() % 6);
                    }
                    g_state->expression.store(next, std::memory_order_relaxed);
                    ESP_LOGI(kTag, "shake |a|=%.2fg → expression %d", mag, next);
                    if (g_board != nullptr) (void)g_board->vibrate(60);
                }
            }
        }
        // BtnB (StopWatch Blue / G1) — manual expression cycle with haptic
        // confirmation. wasPressed() is false on boards without BtnB so the
        // check is harmless universally.
        if (M5.BtnB.wasPressed()) {
            const int cur = g_state->expression.load(std::memory_order_relaxed);
            g_state->expression.store((cur + 1) % 6, std::memory_order_relaxed);
            if (g_board != nullptr) (void)g_board->vibrate(30);
        }
        // BtnA on StopWatch (= Yellow / G2) — toggle the device_ui open/close.
        // The corner tap-to-open hot zone on a round AMOLED is awkward to hit
        // (corners are within the visible circle but at the edge of the touch
        // pad), so a physical button is a more reliable opener. On other
        // boards BtnA either has no role (CoreS3 uses touch only) or is
        // already claimed by atom_status::poll_button (AtomS3R / AtomS3 →
        // is_atom_nyan branch above, which doesn't reach this path).
        if (g_board != nullptr &&
            g_board->kind() == stackchan::board::BoardKind::StopWatch &&
            M5.BtnA.wasPressed()) {
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
                const bool ui_before = app::ui::active();
                app::ui::handle_tap(td.x, td.y);
                // A tap that didn't open/use the on-device UI, while the
                // assistant is mid-reply, is a barge-in request: voice input is
                // paused for the whole turn, so the screen tap is how the user
                // interrupts. The conversation task consumes this during
                // playback.
                if (!ui_before && !app::ui::active() &&
                    g_state->barge_in_enabled.load(std::memory_order_relaxed) &&
                    g_state->conversation_active.load(std::memory_order_relaxed) &&
                    !g_state->conversation_idle.load(std::memory_order_relaxed)) {
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
            if (g_board != nullptr && g_board->kind() == stackchan::board::BoardKind::StopWatch) {
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
                        g_state->gaze_target_h.store(dx * inv * kGazeGain,
                                                     std::memory_order_relaxed);
                        g_state->gaze_target_v.store(dy * inv * kGazeGain,
                                                     std::memory_order_relaxed);
                        follow_active = true;
                    }
                }
                if (!follow_active) {
                    g_state->gaze_target_h.store(0.0f, std::memory_order_relaxed);
                    g_state->gaze_target_v.store(0.0f, std::memory_order_relaxed);
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
            // When idle jtts babble is enabled, drive the mouth from the
            // speech envelope and run the Wi-Fi check + random babble. When
            // disabled, demo_loop becomes a no-op on the mouth so the mic
            // lip-sync task (main/mic_lip_sync_task.cpp), if active, owns
            // `mouth_open` without us overwriting it with 0 every tick.
            if (jtts_idle_enabled) {
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

    // NOTE: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y a freshly-OTA'd image
    // boots in ESP_OTA_IMG_PENDING_VERIFY and must be promoted to VALID or the
    // bootloader rolls it back on the next reset. We deliberately do NOT mark
    // it here — that would defeat the rollback by validating before we know the
    // core came up. The promotion happens after core init completes; see the
    // ESP_OTA_IMG_PENDING_VERIFY self-test at the "ready" point below.

    xTaskCreatePinnedToCore(heap_monitor_task, "heap_mon", 3072, nullptr, 1, nullptr, 1);

    auto board_result = stackchan::board::Board::begin();
    if (!board_result) {
        ESP_LOGE(kTag, "Board::begin() failed: %d", static_cast<int>(board_result.error()));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    auto& board = *board_result;
    g_board = &board;

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
    // NVS init + config load — needed here (earlier than the rest of the
    // setup) so the audio_output decision below can consult cfg.
    {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
    }
    static stackchan::config::DeviceConfig cfg = stackchan::config::load();

    // Cache the jtts options used by the settings-page test-speak buttons
    // (BLE chr 0x2d + /api/jtts-say). Same JSON the demo_loop babble
    // parses, so the test voice matches the babble voice.
    g_say_opts = stackchan::app::resolve_speech_options(
        cfg.jtts_config_json, stackchan::app::Speech::kSampleRate);
    g_say_opts_ready = true;

    // Module Audio (M144, ES8388 codec): probe once at boot. With its
    // jumpers in Config B the codec listens on the M-BUS I2S; the host
    // re-routes its single I2S to the module's pins below (the codec
    // and the internal AW88298 / ES7210 can't run at the same time —
    // they share G0 / G14). cfg.audio_output decides which side wins:
    //   Auto         → codec_present ? module : internal
    //   Internal     → internal even if codec_present
    //   ModuleAudio  → module if codec_present, else warn and use internal
    const bool codec_present = stackchan::board::es8388::probe();
    const bool effective_audio_module =
        codec_present && (cfg.audio_output == stackchan::config::AudioOutput::Auto ||
                          cfg.audio_output == stackchan::config::AudioOutput::ModuleAudio);
    if (cfg.audio_output == stackchan::config::AudioOutput::ModuleAudio && !codec_present) {
        ESP_LOGW(kTag, "audio_output=ModuleAudio but ES8388 absent — falling back to internal");
    }
    if (codec_present && cfg.audio_output == stackchan::config::AudioOutput::Internal) {
        ESP_LOGI(kTag, "Module Audio (ES8388) detected but audio_output=Internal — internal speaker");
    }
    if (effective_audio_module) {
        if (auto r = stackchan::board::es8388::init(); r) {
            ESP_LOGI(kTag, "Module Audio (ES8388) detected — line-out enabled");
        } else {
            ESP_LOGW(kTag, "Module Audio (ES8388) detected but init failed");
        }
    }
    // Keep the old name available so the rest of this block (still using
    // has_audio_module) reads naturally. Equivalent to effective_audio_module.
    const bool has_audio_module = effective_audio_module;
    // Always run the MCU-side RGB diagnostic when EITHER the codec OR the
    // MCU shows up. The MCU lives at 0x33 (LED strip + buttons) and is on
    // the same M-BUS I2C as the codec but doesn't depend on it — useful for
    // bisecting "codec didn't probe but MCU does" cases (jumper / power
    // wiring issue on the codec side specifically).
    if (m5::In_I2C.scanID(stackchan::board::es8388::kMcuI2cAddress, 100'000)) {
        ESP_LOGI(kTag, "Module Audio MCU ACK at 0x%02X — running LED diag",
                 stackchan::board::es8388::kMcuI2cAddress);
        (void)stackchan::board::es8388::diagnose_rgb_pattern();
        if (auto hp = stackchan::board::es8388::headphone_inserted(); hp) {
            ESP_LOGI(kTag, "Module Audio HP jack: %s",
                     *hp ? "inserted" : "not inserted");
        }
    } else if (codec_present) {
        ESP_LOGW(kTag, "Module Audio MCU absent at 0x%02X but codec is present",
                 stackchan::board::es8388::kMcuI2cAddress);
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
        // StopWatch (C152) needs a software gain bump: the M5Unified default
        // spk_cfg.magnification = 1 leaves audio inaudibly quiet through the
        // ES8311 → AW8737A path (R57/R58 = 200 kΩ input attenuator on the
        // amp side eats ~-8 dB before the speaker). Other boards keep the
        // factory magnification (CoreS3 / AtomNyan tune theirs in M5Unified
        // per their codec / amp pair).
        if (board.kind() == stackchan::board::BoardKind::StopWatch) {
            spk.magnification = 16;
        }
        if (has_audio_module) {
            // ES8388 has no amp; its LOUT1/2 jack drive is line-level.
            // The factory CoreS3 magnification (4) plus AW88298's class-D
            // gain assumed the internal speaker downstream — Module
            // Audio's TRRS load needs more digital pre-gain to reach the
            // same perceived loudness through a passive headphone or a
            // typical 0.5 W powered speaker. 8 doubles the headroom
            // without pushing every sample to full scale (which would
            // soft-clip the M5Unified mixer when multiple sources
            // overlap, e.g. JTTS + LT chime).
            spk.magnification = 8;
        }
        // Module Audio (ES8388) uses a COMPLETELY DIFFERENT I2S pinout from
        // CoreS3's internal AW88298 / ES7210, so when the module is fitted
        // we have to re-route the (single) M5.Speaker I2S to its pads. The
        // original hpp header claim that "Config B shares BCK=34 / WS=33 /
        // DIN=13 with the internal codec" was wrong (verified against the
        // M144 schematic + ES7210 conflict on G0 / G14). With these
        // overrides:
        //   - internal AW88298 (BCK=34 / WS=33 / DOUT=13) stops getting an
        //     I2S signal → silent (acceptable; Module Audio is the only
        //     speaker path while fitted)
        //   - internal ES7210 mic (mck=0 / bck=34 / ws=33 / dout = 13) can no
        //     longer record because its pins are now driven by Module
        //     Audio's signals. M5Unified's CoreS3 mic_enable_cb still runs
        //     on M5.Mic.begin() but is a harmless I2C side-effect — no
        //     audio data reaches the chip
        //   - Module Audio gets MCLK on GPIO7, BCK on GPIO0, WS on GPIO6,
        //     DOUT (ESP→codec, labelled "DIN" on the M144 connector) on GPIO14
        // After this M5.Speaker.tone() / playRaw() comes out of the
        // module's 3.5 mm jack (the codec has no on-board amp, so an
        // active speaker / headphones must be plugged in).
        // Phase 1 policy: any module presence flips to Module Audio (internal
        // speaker stays silent for the whole session).
        //
        // Phase 2 (TODO, not implemented yet): hot-switch based on HP-jack
        // insertion (MCU 0x33 reg 0x20). Outline:
        //   - poll headphone_inserted() at ~2 Hz from demo_loop
        //   - on transition: M5.Speaker.end() → rewrite spk.pin_* (internal
        //     pins when unplugged, module pins when plugged) →
        //     M5.Speaker.config(spk). Next playRaw / tone lazy-inits
        //   - watch out for two failure modes: (a) re-route mid-playback
        //     truncates the current segment — gate the switch on
        //     M5.Speaker.isPlaying() == 0, (b) ES8388 init persists across
        //     end()/begin() since registers are sticky once the codec rail
        //     is powered, no need to re-run es8388::init() per switch.
        //   - keep the I2S port the same (I2S_NUM_1) so we don't fight the
        //     M5Unified mic instance over the I2S peripheral
        if (has_audio_module) {
            spk.pin_mck       = GPIO_NUM_7;
            spk.pin_bck       = GPIO_NUM_0;
            spk.pin_ws        = GPIO_NUM_6;
            // M144 connector labels signals from the ESP's perspective:
            // "DOUT G13" = ESP's data-out (i.e. to the codec's input).
            // G13 is the same physical pin the internal AW88298 expects
            // for its DIN, but the two codecs see different BCK / WS
            // pins (Module Audio = G0/G6, internal = G34/G33) so only
            // one decodes the I2S stream at a time — whichever pair
            // matches the current pin config.
            spk.pin_data_out  = GPIO_NUM_13;
        }
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
    // Volume default is per-board. StopWatch's ES8311 + AW8737A path
    // ships with M5Unified's spk_cfg.magnification=1, considerably quieter
    // out-of-the-box than CoreS3's full-range AXP2101+ES8311 path or the
    // AtomNyan ECHO BASE chain. Bump to the top end so the bench unit's
    // 8Ω/1W speaker is actually audible; other boards keep the historical
    // 128 (≈ 50%) which was comfortable on a desk.
    // Module Audio (M144) has NO on-board amp — ES8388 drives the TRRS
    // jack directly at line level, so we need full digital gain on the
    // ESP side. Otherwise the user has to crank their downstream
    // powered-speaker / headphone amp uncomfortably high.
    // Per-board factory volume (the "100%" reference for the user gain
    // slider). StopWatch / Module Audio paths are weaker downstream so
    // they ship at full digital scale; everything else starts at half.
    const std::uint8_t spk_base_volume =
        (board.kind() == stackchan::board::BoardKind::StopWatch || has_audio_module)
            ? 255
            : 128;
    g_speaker_base_volume = spk_base_volume;
    // Apply the user's gain (cfg already loaded above). 0..200 % → byte
    // is clamped at 255 so boards already at 255 are unchanged at 100 %.
    apply_speaker_volume(cfg.speaker_volume_pct);
    if (cfg.startup_arpeggio_enabled) {
        for (float freq : {523.25f, 659.25f, 783.99f}) { // C5 – E5 – G5
            M5.Speaker.tone(freq, 150);
            vTaskDelay(pdMS_TO_TICKS(180));
        }
        while (M5.Speaker.isPlaying()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    } else {
        ESP_LOGI(kTag, "startup arpeggio disabled");
    }
    // JTTS-rate playRaw probe: synth a brief 16 kHz 440 Hz tone and push
    // it through M5.Speaker.playRaw. JTTS babble plays at the same
    // sample rate so this isolates "JTTS-specific failure" from
    // "16 kHz-playRaw-specific failure" — if this probe is audible
    // through Module Audio, the JTTS silence is downstream of sample
    // rate / playRaw path (we'd look at speech.cpp config / mic
    // contention instead). If this probe is also silent while the
    // arpeggio (48 kHz tone) was audible, the codec / I2S clock chain
    // doesn't like the 16 kHz fs.
    {
        constexpr std::uint32_t kProbeRate = 16'000;
        constexpr std::size_t kProbeSamples = kProbeRate * 300 / 1000; // 300 ms
        static std::int16_t probe_pcm[kProbeSamples];
        constexpr float kProbeFreq = 440.0f;
        constexpr float kTwoPi = 6.28318530718f;
        for (std::size_t i = 0; i < kProbeSamples; ++i) {
            probe_pcm[i] = static_cast<std::int16_t>(
                20000.0f * std::sin(kTwoPi * kProbeFreq * static_cast<float>(i) /
                                    static_cast<float>(kProbeRate)));
        }
        ESP_LOGI(kTag, "16 kHz playRaw probe (300 ms, 440 Hz)");
        M5.Speaker.playRaw(probe_pcm, kProbeSamples, kProbeRate, /*stereo=*/false);
        while (M5.Speaker.isPlaying()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    M5.Speaker.end();
    vTaskDelay(pdMS_TO_TICKS(20));

    // nvs_flash_init + config::load have already been called above
    // (needed earlier so cfg.audio_output could gate the Module Audio
    // pin override). `cfg` here is the static instance from line ~840.

    // operation_mode is the single source of truth for the avatar's primary
    // behaviour. Derive the legacy gates from it so the rest of the boot
    // sequence (audio_stream / wifi_audio / conversation task spawn /
    // demo_loop babble decision / mic lip-sync task spawn) keeps its
    // existing shape. See config_service::OperationMode for the enum.
    switch (cfg.operation_mode) {
    case stackchan::config::OperationMode::Conversation:
        cfg.openai_enabled = true;
        cfg.jtts_idle_enabled = false;
        break;
    case stackchan::config::OperationMode::JttsRandom:
        cfg.openai_enabled = false;
        cfg.jtts_idle_enabled = true;
        break;
    case stackchan::config::OperationMode::MicLipSync:
        cfg.openai_enabled = false;
        cfg.jtts_idle_enabled = false;
        break;
    }
    ESP_LOGI(kTag, "operation_mode=%u (conv=%d jtts_idle=%d)",
             static_cast<unsigned>(cfg.operation_mode),
             static_cast<int>(cfg.openai_enabled),
             static_cast<int>(cfg.jtts_idle_enabled));

    // SharedState + audio_stream sink must be live BEFORE config::start
    // brings the BLE GATT service online. Otherwise a client that
    // connects early (well within the 5–10 s of Wi-Fi / mic / servo
    // bring-up that follows) sends `begin` to an unregistered sink and
    // the entire audio session is silently dropped — every subsequent
    // audio_data write sees g_audio_sink == nullptr and bails.
    g_state = new stackchan::app::SharedState{};
    // Seed the speaker volume atom from NVS. apply_speaker_volume was
    // already called before g_state existed (the boot arpeggio block,
    // line ~1093) so M5.Speaker is already at the right level — we just
    // need the atom mirrored so the BLE / WiFi getters and the
    // device-UI row read the correct value when the user first opens
    // a settings page.
    g_state->speaker_volume_pct.store(cfg.speaker_volume_pct, std::memory_order_relaxed);
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
    // Mic lip-sync calibration: seed atomics from NVS. The mic task reads
    // them every loop iteration so slider changes take effect immediately.
    g_state->mic_lip_input_gain_pct.store(
        cfg.mic_lip_input_gain_pct ? cfg.mic_lip_input_gain_pct : 100,
        std::memory_order_relaxed);
    g_state->mic_lip_output_gain_pct.store(
        cfg.mic_lip_output_gain_pct ? cfg.mic_lip_output_gain_pct : 100,
        std::memory_order_relaxed);
    g_state->led_mouth_sync_enabled.store(cfg.led_mouth_sync_enabled,
                                          std::memory_order_relaxed);
    g_state->lip_sync_mode.store(static_cast<std::uint8_t>(cfg.lip_sync_mode),
                                 std::memory_order_relaxed);
    g_state->mic_lip_agc_enabled.store(cfg.mic_lip_agc_enabled,
                                       std::memory_order_relaxed);
    g_state->barge_in_enabled.store(cfg.barge_in_enabled,
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
    stackchan::config::set_mic_lip_gain_getter(&read_mic_lip_gain);
    stackchan::config::set_mic_lip_gain_sink(&apply_mic_lip_gain);
    stackchan::config::set_speaker_volume_getter(&read_speaker_volume_pct);
    stackchan::config::set_speaker_volume_sink(&apply_speaker_volume_sink);
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
    stackchan::wifi_config::set_mic_lip_gain_getter(&read_mic_lip_gain);
    stackchan::wifi_config::set_mic_lip_gain_sink(&apply_mic_lip_gain);
    stackchan::wifi_config::set_speaker_volume_getter(&read_speaker_volume_pct);
    stackchan::wifi_config::set_speaker_volume_sink(&apply_speaker_volume_sink);
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

#if CONFIG_STACKCHAN_CAMERA_ENABLED
    // One-shot camera capture for the settings page (GET /api/camera/capture).
    // Only registered when the board actually wires the GC0308 (M5 CoreS3
    // base) — /api/status's has_camera flag derives from this registration,
    // which is how the web UI decides to show the 撮影 section.
    if (board.kind() == stackchan::board::BoardKind::M5Base) {
        stackchan::wifi_config::set_camera_capture_sink(
            [](std::vector<std::uint8_t>& out, std::size_t& w, std::size_t& h) -> bool {
                // The QR worker owns the camera while scanning (fb_count=1,
                // single esp_camera instance) — don't double-init under it.
                if (stackchan::app::qr_scan_active()) {
                    ESP_LOGW(kTag, "camera capture rejected: QR scan active");
                    return false;
                }
                stackchan::board::CameraGc0308 camera;
                if (auto r = camera.begin(); !r) {
                    ESP_LOGE(kTag, "camera capture: begin failed: %d",
                             static_cast<int>(r.error()));
                    return false;
                }
                // Discard the first frames: the sensor's AGC/AWB starts from
                // reset defaults and the first ~4 frames are badly under- or
                // over-exposed. ~5 frames ≈ 300 ms at the GC0308's QVGA rate.
                for (int i = 0; i < 5; ++i) {
                    (void)camera.capture();
                }
                bool ok = false;
                {
                    // Scoped so ~CameraFrame returns the fb to the driver
                    // BEFORE camera.end() tears the driver down.
                    auto frame = camera.capture();
                    if (frame) {
                        out.assign(frame->data(), frame->data() + frame->size());
                        w = frame->width();
                        h = frame->height();
                        ok = true;
                    } else {
                        ESP_LOGE(kTag, "camera capture failed: %d",
                                 static_cast<int>(frame.error()));
                    }
                }
                (void)camera.end();
                return ok;
            });
    }
#endif

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
        [](std::string_view kana_utf8) { start_say_worker(kana_utf8); });
    // Settings-page "speak this" test buttons — same body as /mcp/say
    // but gated by the page's existing auth (BLE session crypto or
    // HTTP Basic) instead of the MCP token, so the owner can test
    // jtts without setting up MCP.
    stackchan::config::set_jtts_say_kana_sink(&start_say_worker);
    stackchan::wifi_config::set_jtts_say_kana_sink(&start_say_worker);

    // Wi-Fi live audio (RTP/L16 today). Like the BLE sink, mutually exclusive
    // with the conversation backend, so it self-disables when voice chat is on.
#if CONFIG_STACKCHAN_WIFI_AUDIO_ENABLED
    stackchan::app::wifi_audio::start(*g_state, cfg.openai_enabled, cfg.rtp_audio_enabled);
#else
    ESP_LOGI(kTag, "wifi_audio: disabled at compile time (slim build)");
#endif

    // Mic / loopback sanity check at startup — DISABLED. Calling
    // M5.Mic.record here reconfigures the shared I2S_NUM_1 peripheral
    // onto the internal ES7210 pin set (G0=MCLK / G34=BCK / G33=WS /
    // G14=DIN), which collides with the Module Audio pin override above
    // (G0=BCK / G14=DIN-mic-side). Even though the next Speaker.begin
    // restores the Module Audio pins from spk_cfg, the side effect of
    // having driven G0 as MCLK for the 2 s recording window appears to
    // leave the ES8388 in a state where later 16 kHz playRaw silently
    // drops samples (boot arpeggio at 48 kHz still works, JTTS at
    // 16 kHz doesn't). The mic loopback was a Phase-1 bring-up tool;
    // not needed in normal operation.
    // record_and_playback(2, "mic test");

    // is_atom_nyan: legacy predicate covering "no servo bus + no LCD touch
    // → use atom_status button overlay UI". AtomS3R / AtomS3 fall here.
    // StopWatch ALSO has no servo bus but DOES have a CST820B touch panel,
    // so it gets the CoreS3-style touch UI (ui::handle_tap) below — kept
    // separate from this flag. Servo gating uses the wider no_servo_bus
    // below; only the on-device UI choice keys off is_atom_nyan.
    const bool is_atom_nyan =
        board.kind() == stackchan::board::BoardKind::AtomNyan ||
        board.kind() == stackchan::board::BoardKind::AtomS3;
    // Boards without an SCS servo bus. Covers Atom family + StopWatch (no
    // M5 base wiring) — used to gate servo-power bring-up, servo task
    // start-up, and barge-in touch hit-tests that assume a head with
    // servos.
    //
    // Module Audio (M144) is also lumped in here: M5Base's SCS bus uses
    // UART1 on G6 (TX) / G7 (RX), and the Module Audio I2S output we
    // configure overrides those same pads (G6 = WS, G7 = MCLK). The
    // ESP32-S3 GPIO matrix is 1-signal-per-pad, so once M5.Speaker.begin
    // claims the pins (boot arpeggio, then every jtts / conv / MCP say)
    // the servo bus stops responding — ping / read_present_position /
    // torque-enable all fail, range mode never publishes positions and
    // can't re-engage torque on exit. Hard-gate everything servo-related
    // off when the Module Audio path is active so the user is left in a
    // predictable "silent head" state rather than partially-broken
    // servos. The user can pick audio_output=Internal to recover servo.
    const bool no_servo_bus =
        is_atom_nyan ||
        board.kind() == stackchan::board::BoardKind::StopWatch ||
        effective_audio_module;

    // Servo bring-up is only meaningful on boards that actually have a servo
    // bus (CoreS3 + M5/Takao). Atom-nyan has no servos in Phase 1 scope; skip
    // both the power-rail enable and the 1.5 s settle wait. Also gated by the
    // NVS-persisted master switch cfg.servo_enabled (settable from BLE / Wi-Fi
    // / on-device UI — distinct from SharedState::servo_enabled which is the
    // live torque toggle).
    if (!no_servo_bus && cfg.servo_enabled) {
        if (auto r = board.set_servo_power(true); !r) {
            ESP_LOGE(kTag, "set_servo_power(true) failed: %d", static_cast<int>(r.error()));
        }
        // Allow the servo bus rail to settle before the servo task starts
        // driving UART. SCS0009 needs ~1 s after Vmotor comes up before it
        // answers PING.
        vTaskDelay(pdMS_TO_TICKS(1500));
        // Servo VM coming up is a known Si12T baseline disturbance: the
        // chip's running baseline acquired with Vmotor off no longer
        // matches the post-power-on environment, which we've seen as
        // ghost head-touch firings in the first few seconds. Force a
        // baseline update now (cheap: 4 I2C writes) so the chip starts
        // clean instead of waiting for FTC=10 s to drift back.
        if (auto* t = board.touch_sensor(); t != nullptr) {
            t->recalibrate();
        }
    } else if (!cfg.servo_enabled) {
        ESP_LOGW(kTag, "servo VM rail OFF: cfg.servo_enabled=false (set via settings UI)");
    }

    const bool is_circular_display =
        board.kind() == stackchan::board::BoardKind::StopWatch;
    g_render_args = new stackchan::app::RenderTaskArgs{
        .display = &board.display(),
        .state = g_state,
        .circular_display = is_circular_display,
    };
    const auto servo_limits = stackchan::app::parse_servo_limits(cfg.servo_limits_json);
    if (!no_servo_bus) {
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
    if (!no_servo_bus && cfg.servo_enabled && g_servo_args != nullptr) {
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

    // Mic-driven lip sync. Activates only when BOTH the conversation backend
    // AND the jtts idle babble are off — that way nothing else is driving
    // `mouth_open` and the I2S bus stays free for the mic to own. The task
    // yields to any speaker activity (balloon say / MCP say / OTA chime) and
    // re-acquires the mic afterwards. See main/mic_lip_sync_task.cpp.
    if (!cfg.openai_enabled && !cfg.jtts_idle_enabled) {
        ESP_LOGI(kTag, "mic lip-sync: starting (conversation off, jtts idle off)");
        stackchan::app::start_mic_lip_sync_task(*g_state);
    } else {
        ESP_LOGI(kTag, "mic lip-sync: not started (conv=%d jtts_idle=%d)",
                 static_cast<int>(cfg.openai_enabled),
                 static_cast<int>(cfg.jtts_idle_enabled));
    }

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

    // OTA rollback self-test. If we're running a freshly-OTA'd image the
    // bootloader left it in ESP_OTA_IMG_PENDING_VERIFY; reaching here means
    // core init (Board::begin / NVS / config load / render + servo + LED tasks)
    // completed without a boot-loop crash, so promote the image to VALID and
    // cancel the pending rollback. Runs on the app_main task, whose stack is in
    // internal RAM — required because esp_ota_* touches flash / NVS with the
    // instruction cache disabled and a PSRAM stack would be unreadable there.
    // We wait a short beat first so the tasks we just spawned get a chance to
    // run (and crash, if the image is bad) before we commit. Only images in
    // PENDING_VERIFY are promoted; a normal boot from a VALID / factory image
    // is a no-op.
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
        if (running != nullptr &&
            esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_err_t verr = esp_ota_mark_app_valid_cancel_rollback();
            if (verr == ESP_OK) {
                ESP_LOGI(kTag, "OTA self-test passed — image marked VALID (rollback cancelled)");
            } else {
                ESP_LOGE(kTag, "esp_ota_mark_app_valid_cancel_rollback failed: %s",
                         esp_err_to_name(verr));
            }
        }
    }

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
              cfg.openai_enabled, cfg.jtts_idle_enabled, servo_limits);
}
