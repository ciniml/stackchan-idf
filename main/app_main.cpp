#include <cmath>
#include <cstdint>
#include <vector>

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "avatar/expression.hpp"
#include "board/board.hpp"
#include "render_task.hpp"
#include "servo_task.hpp"
#include "shared_state.hpp"
#include "speech.hpp"

namespace {

constexpr const char* kTag = "stackchan";

// Heap-allocate so the task argument outlives app_main's scope (the tasks run forever).
stackchan::app::SharedState* g_state = nullptr;
stackchan::app::RenderTaskArgs* g_render_args = nullptr;
stackchan::app::ServoTaskArgs* g_servo_args = nullptr;

void demo_loop()
{
    using namespace stackchan;

    constexpr avatar::Expression kCycle[] = {
        avatar::Expression::Neutral, avatar::Expression::Happy, avatar::Expression::Doubt,
        avatar::Expression::Sad,     avatar::Expression::Angry, avatar::Expression::Sleepy,
    };

    // Random head pose targets, redrawn every kPoseMinMs..kPoseMaxMs.
    constexpr float kYawMaxDeg = 40.0f;          // ±40°
    constexpr float kPitchMinDeg = -10.0f;       // little down
    constexpr float kPitchMaxDeg =  25.0f;       // more up
    constexpr std::uint32_t kPoseMinMs = 10000;
    constexpr std::uint32_t kPoseMaxMs = 20000;
    constexpr std::uint32_t kExpressionPeriodMs = 5000;
    constexpr std::uint32_t kSpeechMinMs = 6000;
    constexpr std::uint32_t kSpeechMaxMs = 12000;

    static constexpr const char* kPhrases[] = {
        "Hello!",
        "Hi there",
        "How are you?",
        "I'm listening",
        "Tell me more",
        "Welcome to Stack-chan firmware on ESP-IDF 5.4",
        "Did you know I have two servos and one face?",
        "こんにちは",
        "おはよう",
        "今日もよろしくね",
        "ピッ ポッ ピッ",
        "スタックチャンです、よろしく!",
        "ESP-IDF 5.4 でうごいてます",
        "サーボとアバターのテスト中です",
    };

    static app::Speech speech;

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

    // Set true by the (render-task) completion callback so demo_loop knows
    // the previous balloon finished. Atomics keep it thread-safe.
    static std::atomic<bool> balloon_in_flight{false};

    for (;;) {
        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);

        // Mouth opens with the current speech envelope; closed while silent.
        g_state->mouth_open.store(speech.current_mouth_open(), std::memory_order_relaxed);

        // Kick off a new babble + balloon once the previous balloon is done
        // (callback resets balloon_in_flight) AND audio is idle AND the random
        // dwell time has elapsed.
        if (now_ms >= next_speech_ms &&
            !speech.is_speaking() &&
            !balloon_in_flight.load(std::memory_order_acquire)) {
            speech.babble(now_ms);
            constexpr std::size_t kPhraseCount = sizeof(kPhrases) / sizeof(kPhrases[0]);
            const char* phrase = kPhrases[esp_random() % kPhraseCount];
            balloon_in_flight.store(true, std::memory_order_release);
            g_state->set_balloon_text(phrase, /*hold_ms=*/0, [] {
                balloon_in_flight.store(false, std::memory_order_release);
            });
            next_speech_ms = now_ms + rand_range_ms(kSpeechMinMs, kSpeechMaxMs);
        }

        // Random yaw + pitch every 10–20 s.
        if (now_ms >= next_pose_ms) {
            g_state->target_yaw_deg.store(rand_in(-kYawMaxDeg, kYawMaxDeg), std::memory_order_relaxed);
            g_state->target_pitch_deg.store(rand_in(kPitchMinDeg, kPitchMaxDeg), std::memory_order_relaxed);
            next_pose_ms = now_ms + rand_range_ms(kPoseMinMs, kPoseMaxMs);
        }

        // Cycle expression every 5 s.
        if (now_ms >= next_expression_ms) {
            g_state->expression.store(static_cast<int>(kCycle[expression_index]), std::memory_order_relaxed);
            expression_index = (expression_index + 1) % (sizeof(kCycle) / sizeof(kCycle[0]));
            next_expression_ms = now_ms + kExpressionPeriodMs;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace

extern "C" void app_main()
{
    auto board_result = stackchan::board::Board::begin();
    if (!board_result) {
        ESP_LOGE(kTag, "Board::begin() failed: %d", static_cast<int>(board_result.error()));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    auto& board = *board_result;

    // CoreS3 Speaker and Mic share I2S_NUM_1 (BCK=GPIO34, WS=GPIO33),
    // so the side that's done has to release the bus before the other
    // side can install its own driver.

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

    // Mic / loopback sanity check: record 2 s, then play it back so we can
    // hear whether the on-board mic + ES7210 codec are wired up.
    {
        constexpr std::uint32_t kSampleRate = 16'000;
        constexpr std::uint32_t kSeconds = 2;
        constexpr std::size_t kSamples = kSampleRate * kSeconds;
        std::vector<std::int16_t> buf(kSamples, 0);

        ESP_LOGI(kTag, "mic test: recording %u s...", static_cast<unsigned>(kSeconds));
        if (!M5.Mic.record(buf.data(), buf.size(), kSampleRate, /*stereo=*/false)) {
            ESP_LOGE(kTag, "M5.Mic.record returned false");
        } else {
            // record() is async — the mic task hasn't necessarily flipped
            // _is_recording yet. Spin a few ticks until it has, then wait
            // for it to drop back to 0.
            for (int i = 0; i < 50 && M5.Mic.isRecording() == 0; ++i) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            while (M5.Mic.isRecording()) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            M5.Mic.end();
            vTaskDelay(pdMS_TO_TICKS(20));
            ESP_LOGI(kTag, "mic test: playing back...");
            M5.Speaker.playRaw(buf.data(), buf.size(), kSampleRate, /*stereo=*/false);
            while (M5.Speaker.isPlaying()) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            ESP_LOGI(kTag, "mic test: done");
        }
    }

    if (auto r = board.set_servo_power(true); !r) {
        ESP_LOGE(kTag, "set_servo_power(true) failed: %d", static_cast<int>(r.error()));
    }
    // Allow the servo bus rail to settle before the servo task starts driving
    // UART. SCS0009 needs ~1 s after Vmotor comes up before it answers PING.
    vTaskDelay(pdMS_TO_TICKS(1500));

    g_state = new stackchan::app::SharedState{};
    g_render_args = new stackchan::app::RenderTaskArgs{.display = &board.display(), .state = g_state};
    g_servo_args = new stackchan::app::ServoTaskArgs{.state = g_state};

    stackchan::app::start_render_task(*g_render_args);
    stackchan::app::start_servo_task(*g_servo_args);

    ESP_LOGI(kTag, "ready");
    demo_loop();
}
