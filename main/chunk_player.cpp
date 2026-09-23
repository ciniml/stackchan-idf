// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "chunk_player.hpp"

#include <algorithm>

#include <M5Unified.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {

// M5.Speaker の 1 チャンネルのスロット数 (再生中 + 次)。
constexpr std::size_t kSlots = 2;

// 折り返しを考慮して a が b より後か。
bool after(std::uint32_t a, std::uint32_t b)
{
    return static_cast<std::int32_t>(a - b) > 0;
}

} // namespace

std::uint32_t now_ms()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

void ChunkPlayer::begin()
{
    const auto ch = static_cast<std::uint8_t>(channel_);
    bool was_playing = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        was_playing = end_ms_ != 0 && after(end_ms_, now_ms());
        if (was_playing || M5.Speaker.isPlaying(ch) != 0) {
            M5.Speaker.stop(ch);
            was_playing = true;
        }
        end_ms_ = 0;
    }
    if (was_playing) {
        // スピーカー タスクが最後のブロックを読み終えるまで待ってから解放する。
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    release();
}

std::optional<std::uint32_t> ChunkPlayer::enqueue(std::vector<std::int16_t>&& pcm, std::uint32_t sample_rate,
                                                  const std::function<bool()>& cancelled)
{
    const auto is_cancelled = [&] { return cancelled && cancelled(); };
    if (pcm.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        return std::max(now_ms(), end_ms_);
    }
    const auto ch = static_cast<std::uint8_t>(channel_);

    // 空きスロットが出るまで待つ。合成の方が再生より速いとここで待たされるが、
    // その分メモリを溜め込まない (最大でも「再生中 + 次 + 合成中」の 3 チャンク)。
    while (M5.Speaker.isPlaying(ch) >= kSlots) {
        if (is_cancelled()) return std::nullopt;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 「取り消し確認 → 再生開始」を stop() と直列化する。stop() はこのロックの下で
    // 取り消しフラグを立ててスピーカーを止めるので、確認を通ったチャンクは必ず
    // stop() より前に鳴り始めていて、stop() がそれも止める。
    std::lock_guard<std::mutex> lock(mtx_);
    if (is_cancelled()) return std::nullopt;

    bufs_.push_back(std::move(pcm));
    const std::vector<std::int16_t>& buf = bufs_.back();
    while (!M5.Speaker.playRaw(buf.data(), buf.size(), sample_rate, /*stereo=*/false,
                               /*repeat=*/1, channel_, /*stop_current_sound=*/false)) {
        vTaskDelay(pdMS_TO_TICKS(10)); // スロットが埋まっていた (稀): 空くまで再試行
    }

    // 再生予定: 直前のチャンクがまだ鳴っていればその直後、途切れていれば今。
    const std::uint32_t now = now_ms();
    const std::uint32_t start = after(end_ms_, now) ? end_ms_ : now;
    const auto dur_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(buf.size()) * 1000u / sample_rate);
    end_ms_ = start + dur_ms;

    // 鳴らし終えたバッファを解放する。スピーカーがまだ参照しうる分 (再生中 + 次) に
    // 1 つ余裕を足して残す。
    const std::size_t pending = std::max<std::size_t>(M5.Speaker.isPlaying(ch), 1);
    while (bufs_.size() > pending + 1) {
        bufs_.pop_front();
    }
    return start;
}

bool ChunkPlayer::finished() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return end_ms_ == 0 || !after(end_ms_, now_ms());
}

void ChunkPlayer::stop(const std::function<void()>& before)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (before) before();
    const auto ch = static_cast<std::uint8_t>(channel_);
    if (M5.Speaker.isPlaying(ch) != 0) {
        M5.Speaker.stop(ch);
    }
    end_ms_ = 0;
}

void ChunkPlayer::release()
{
    std::lock_guard<std::mutex> lock(mtx_);
    bufs_.clear();
    end_ms_ = 0;
}

} // namespace stackchan::app
