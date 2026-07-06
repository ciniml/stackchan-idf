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
#include "avatar_vm/storage.hpp"
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
#include "camera_service.hpp"
#include "demo_loop.hpp"
#include "device_ui.hpp"
#include "diag.hpp"
#include "i2c_dump.hpp"
#include "led_task.hpp"
#include "mic_lip_sync_task.hpp"
#include "qr_task.hpp"
#include "render_task.hpp"
#include "servo_limits.hpp"
#include "servo_task.hpp"
#include "settings_sinks.hpp"
#include "shared_state.hpp"
#include "speech.hpp"
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

    // Resident camera init (M5Base only; no-op elsewhere / slim builds).
    // Must run after the i2c dump (which uses In_I2C) and before any task
    // that touches In_I2C starts — see camera_service::init_resident.
    stackchan::app::camera_service::init_resident(board);

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
    stackchan::app::settings_sinks::set_say_options(stackchan::app::resolve_speech_options(
        cfg.jtts_config_json, stackchan::app::Speech::kSampleRate));

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
    stackchan::app::settings_sinks::set_speaker_base_volume(spk_base_volume);
    // Apply the user's gain (cfg already loaded above). 0..200 % → byte
    // is clamped at 255 so boards already at 255 are unchanged at 100 %.
    stackchan::app::settings_sinks::apply_speaker_volume(cfg.speaker_volume_pct);
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
    stackchan::app::settings_sinks::attach_state(*g_state);
    // Seed the speaker volume atom from NVS. apply_speaker_volume was
    // already called before g_state existed (the boot arpeggio block,
    // line ~1093) so M5.Speaker is already at the right level — we just
    // need the atom mirrored so the BLE / WiFi getters and the
    // device-UI row read the correct value when the user first opens
    // a settings page.
    g_state->speaker.volume_pct.store(cfg.speaker_volume_pct, std::memory_order_relaxed);
    // Seed the avatar face tuning from NVS (empty → built-in default face) and
    // register the live BLE update sink, both before config::start brings the
    // GATT service online so an early client write is applied immediately.
    g_state->set_face_config(cfg.face_config_json);
    // LT timekeeper config: seeding bumps the version, so demo_loop's poll
    // applies it on its first iteration (no special boot path needed).
    if (!cfg.lt_config_json.empty()) {
        g_state->set_lt_config(cfg.lt_config_json);
    }
    g_state->battery.gauge_enabled.store(cfg.battery_gauge_enabled, std::memory_order_relaxed);
    // LED state: replay the persisted values so the strip lights up the same
    // way it did before the reboot. NVS-missing → DeviceConfig's struct
    // defaults (gradient @ ~10%) so a fresh-install device still glows.
    g_state->led.mode.store(cfg.led_mode, std::memory_order_relaxed);
    g_state->led.color.store(cfg.led_color, std::memory_order_relaxed);
    g_state->led.brightness.store(cfg.led_brightness, std::memory_order_relaxed);
    g_state->led.gradient_period_ds.store(
        cfg.led_gradient_period_ds == 0 ? 1 : cfg.led_gradient_period_ds,
        std::memory_order_relaxed);
    // Mic lip-sync calibration: seed atomics from NVS. The mic task reads
    // them every loop iteration so slider changes take effect immediately.
    g_state->mic_lip.input_gain_pct.store(
        cfg.mic_lip_input_gain_pct ? cfg.mic_lip_input_gain_pct : 100,
        std::memory_order_relaxed);
    g_state->mic_lip.output_gain_pct.store(
        cfg.mic_lip_output_gain_pct ? cfg.mic_lip_output_gain_pct : 100,
        std::memory_order_relaxed);
    g_state->led.mouth_sync_enabled.store(cfg.led_mouth_sync_enabled,
                                          std::memory_order_relaxed);
    g_state->led.lip_sync_mode.store(static_cast<std::uint8_t>(cfg.lip_sync_mode),
                                 std::memory_order_relaxed);
    g_state->mic_lip.agc_enabled.store(cfg.mic_lip_agc_enabled,
                                       std::memory_order_relaxed);
    g_state->barge_in_enabled.store(cfg.barge_in_enabled,
                                    std::memory_order_relaxed);
    stackchan::app::settings_sinks::register_ble_sinks(
        static_cast<std::uint8_t>(board.kind()));
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
    stackchan::app::settings_sinks::register_http_sinks(
        static_cast<std::uint8_t>(board.kind()));
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
    stackchan::app::settings_sinks::register_avatar_bytecode_sinks();

    // Camera capture / register sinks (no-op when the resident camera did
    // not come up; /api/status's has_camera derives from this registration).
    stackchan::app::camera_service::register_sinks(*g_state);

    // /mcp/* channel sinks + the settings-page test-speak buttons.
    stackchan::app::settings_sinks::register_mcp_sinks();

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
    stackchan::board::Si12tTouch* const head_touch = board.touch_sensor();

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
        .state = g_state, .api_key = api_key, .provider = cfg.provider, .touch = head_touch,
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
                                  : static_cast<int>(g_state->conv.status.load(
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

    stackchan::app::run_demo_loop({
        .state = g_state,
        .board = &board,
        .touch = head_touch,
        .jtts_config_json = cfg.jtts_config_json,
        .has_battery = board.has_battery(),
        .is_atom_nyan = is_atom_nyan,
        .conversation_enabled = cfg.openai_enabled,
        .jtts_idle_enabled = cfg.jtts_idle_enabled,
        .limits = servo_limits,
    });
}
