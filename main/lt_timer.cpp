// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "lt_timer.hpp"

#include <cJSON.h>
#include <esp_log.h>

#include "shared_state.hpp"
#include "speech.hpp"
#include "utf8.hpp"

namespace stackchan::app {

namespace {
constexpr const char* kTag = "lt_timer";
constexpr std::uint32_t kBalloonHoldMs = 4000;
} // namespace

void LtTimer::configure(const std::string& json, SharedState* state)
{
    if (json.empty()) return;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "config JSON parse failed — keeping defaults");
        return;
    }
    if (const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, "total_s");
        cJSON_IsNumber(v) && v->valueint > 0 && v->valueint <= 0xFFFF && state != nullptr) {
        // Don't yank a live countdown; the new default applies from the
        // next start. lt_remaining_s doubles as the idle-state display.
        if (!state->lt_active.load(std::memory_order_relaxed)) {
            state->lt_total_s.store(static_cast<std::uint16_t>(v->valueint),
                                    std::memory_order_relaxed);
            state->lt_remaining_s.store(v->valueint, std::memory_order_relaxed);
        } else {
            state->lt_total_s.store(static_cast<std::uint16_t>(v->valueint),
                                    std::memory_order_relaxed);
        }
    }
    if (const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, "warn_s");
        cJSON_IsNumber(v) && v->valueint > 0) {
        cfg_.warn_s = static_cast<std::uint32_t>(v->valueint);
    }
    if (const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, "repeat_s");
        cJSON_IsNumber(v) && v->valueint >= 0) {
        // 0 = announce overtime once, never repeat.
        cfg_.repeat_s = static_cast<std::uint32_t>(v->valueint);
    }
    auto load_phrase = [&](const char* key, std::string& display, std::string& reading) {
        const cJSON* obj = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsObject(obj)) return;
        const cJSON* t = cJSON_GetObjectItemCaseSensitive(obj, "text");
        const cJSON* r = cJSON_GetObjectItemCaseSensitive(obj, "reading");
        if (cJSON_IsString(t) && t->valuestring != nullptr) display = t->valuestring;
        if (cJSON_IsString(r) && r->valuestring != nullptr) reading = r->valuestring;
    };
    load_phrase("warn", cfg_.warn_display, cfg_.warn_reading);
    load_phrase("just", cfg_.just_display, cfg_.just_reading);
    load_phrase("over", cfg_.over_display, cfg_.over_reading);
    cJSON_Delete(root);
    ESP_LOGI(kTag, "config: warn_s=%u repeat_s=%u", static_cast<unsigned>(cfg_.warn_s),
             static_cast<unsigned>(cfg_.repeat_s));
}

void LtTimer::announce(SharedState& state, Speech& speech,
                       const std::string& display, const std::string& reading)
{
    state.set_balloon_text(display, kBalloonHoldMs);
    if (!speech.say(decode_utf8(reading))) {
        ESP_LOGW(kTag, "announcement synthesis failed: %s", reading.c_str());
    }
}

void LtTimer::tick(SharedState& state, Speech& speech, std::uint32_t now_ms)
{
    // Consume a pending UI command (exchange so we never double-run one).
    switch (state.lt_command.exchange(0, std::memory_order_acq_rel)) {
    case 1: { // start
        const std::uint32_t total_s = state.lt_total_s.load(std::memory_order_relaxed);
        deadline_ms_ = now_ms + total_s * 1000U;
        running_ = true;
        warned_ = false;
        just_fired_ = false;
        next_over_ms_ = 0;
        state.lt_active.store(true, std::memory_order_relaxed);
        state.lt_remaining_s.store(static_cast<std::int32_t>(total_s), std::memory_order_relaxed);
        ESP_LOGI(kTag, "start: %u s", static_cast<unsigned>(total_s));
        break;
    }
    case 2: // stop / reset
        if (running_) ESP_LOGI(kTag, "stopped");
        running_ = false;
        state.lt_active.store(false, std::memory_order_relaxed);
        state.lt_remaining_s.store(
            static_cast<std::int32_t>(state.lt_total_s.load(std::memory_order_relaxed)),
            std::memory_order_relaxed);
        break;
    default:
        break;
    }

    if (!running_) return;

    // Signed remaining seconds; negative counts up the overtime. Round
    // toward -inf so the displayed 0:00 lines up with the announcement.
    const std::int64_t remaining_ms =
        static_cast<std::int64_t>(deadline_ms_) - static_cast<std::int64_t>(now_ms);
    const auto remaining_s = static_cast<std::int32_t>(
        remaining_ms >= 0 ? (remaining_ms + 999) / 1000 : remaining_ms / 1000);
    state.lt_remaining_s.store(remaining_s, std::memory_order_relaxed);

    if (!warned_ && remaining_ms <= static_cast<std::int64_t>(cfg_.warn_s) * 1000) {
        warned_ = true;
        announce(state, speech, cfg_.warn_display, cfg_.warn_reading);
    }

    if (remaining_ms <= 0) {
        if (!just_fired_) {
            // First hit at the deadline — use the dedicated `just` phrase
            // when set, otherwise fall back to `over` so legacy configs
            // (no "just" key) sound the same as before.
            just_fired_ = true;
            const bool has_just =
                !cfg_.just_display.empty() || !cfg_.just_reading.empty();
            announce(state, speech,
                     has_just ? cfg_.just_display : cfg_.over_display,
                     has_just ? cfg_.just_reading : cfg_.over_reading);
            if (cfg_.repeat_s == 0) {
                next_over_ms_ = UINT32_MAX;
            } else {
                next_over_ms_ = now_ms + cfg_.repeat_s * 1000U;
            }
        } else if (now_ms >= next_over_ms_) {
            // Subsequent overtime nudges always use `over` regardless of
            // whether `just` was distinct (the deadline message has
            // already played; we want the running "you're over" reminder).
            announce(state, speech, cfg_.over_display, cfg_.over_reading);
            if (cfg_.repeat_s == 0) {
                next_over_ms_ = UINT32_MAX;
            } else {
                next_over_ms_ = now_ms + cfg_.repeat_s * 1000U;
            }
        }
    }
}

} // namespace stackchan::app
