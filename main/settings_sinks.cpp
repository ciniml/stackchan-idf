// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "settings_sinks.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "avatar/expression.hpp"
#include "config_service/config_service.hpp"
#include "config_service/config_store.hpp"
#include "speech.hpp"
#include "utf8.hpp"
#include <wifi_config_service/mcp_events.hpp>
#include <wifi_config_service/wifi_config_service.hpp>

namespace stackchan::app::settings_sinks {

namespace {

constexpr const char* kTag = "stackchan";

SharedState* g_state = nullptr;

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

} // namespace

void attach_state(SharedState& state)
{
    g_state = &state;
}

void set_speaker_base_volume(std::uint8_t base)
{
    g_speaker_base_volume = base;
}

void set_say_options(const stackchan::jtts::Options& opts)
{
    g_say_opts = opts;
    g_say_opts_ready = true;
}

void apply_speaker_volume(std::uint16_t pct) noexcept
{
    if (pct > 200) pct = 200;
    int v = static_cast<int>(g_speaker_base_volume) * static_cast<int>(pct) / 100;
    if (v > 255) v = 255;
    if (v < 0) v = 0;
    // One-touch mute overrides the percent: master volume goes to 0 while
    // the flag is set, and the user's pct survives untouched for unmute.
    if (g_state != nullptr && g_state->speaker_muted.load(std::memory_order_relaxed)) {
        v = 0;
    }
    M5.Speaker.setVolume(static_cast<std::uint8_t>(v));
    if (g_state != nullptr) {
        g_state->speaker_volume_pct.store(pct, std::memory_order_relaxed);
    }
}

void apply_speaker_volume_persist(std::uint16_t pct)
{
    if (pct > 200) pct = 200;
    apply_speaker_volume(pct);  // sets M5.Speaker.setVolume + SharedState
    (void)stackchan::config::store::save_speaker_volume(pct);
}

void register_ble_sinks(std::uint8_t board_kind)
{
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
    stackchan::config::set_speaker_volume_sink(&apply_speaker_volume_persist);
    // Tell the settings service which board we're on so the web UI can hide
    // sections that don't apply (e.g. servo config on Atom-nyan). Must happen
    // before config::start so the first central read sees the right value.
    stackchan::config::set_board_kind(board_kind);
}

void register_http_sinks(std::uint8_t board_kind)
{
    // Same sink/getter set as BLE. The Wi-Fi service starts on a worker task
    // after Wi-Fi STA gets an IP — these calls race that; the setters
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
    stackchan::wifi_config::set_speaker_volume_sink(&apply_speaker_volume_persist);
    stackchan::wifi_config::set_board_kind(board_kind);
}

void register_avatar_bytecode_sinks()
{
    // Same closure for both transports: chr 0x21 (BLE) and POST /api/avatar-dsl
    // (HTTP). The free function decays to a function pointer for the BLE
    // setter, and converts implicitly to std::function for the HTTP one.
    stackchan::config::set_avatar_bytecode_sink(&apply_avatar_bytecode);
    stackchan::wifi_config::set_avatar_bytecode_sink(&apply_avatar_bytecode);
}

void register_mcp_sinks()
{
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
}

} // namespace stackchan::app::settings_sinks
