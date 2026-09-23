// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 合成済み PCM を「チャンク単位で」M5.Speaker の 1 チャンネルに順に積んで再生する。
// jtts::synthesize_stream のシンクから enqueue() を呼ぶと、最初のチャンクは即座に
// 鳴り始め、以降は再生中に次を合成して積める (隙間なく連続再生される)。
//
// M5.Speaker の 1 チャンネルは「再生中 + 次」の 2 スロットを持ち、playRaw は渡された
// バッファを *コピーせず* 再生が終わるまで参照する。そのため enqueue() は
//   - スロットが空くまで待つ (= 合成が再生より先に進みすぎない。メモリも抑えられる)
//   - PCM を保持し、再生し終えたものから順に解放する
// を担う。再生予定の時刻も管理するので、口形イベントの時刻合わせに使える。
//
// スレッド: enqueue() / begin() / release() は 1 つのタスク (合成タスク) からだけ呼ぶ。
// stop() だけは別のタスクから呼んでよい (発話の取り消し)。stop() の後に遅れて
// チャンクが鳴り出さないよう、enqueue() の「取り消し確認 → 再生開始」は stop() と
// 同じミューテックスで直列化している。
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace stackchan::app {

class ChunkPlayer {
public:
    // 使うスピーカー チャンネル (会話の再生 = 0 とは別にして互いの再生状態に干渉しない)。
    explicit ChunkPlayer(int channel) : channel_(channel) {}

    // 新しい発話を始める: 前の発話がまだ鳴っていれば止め、保持しているバッファを全て解放する。
    void begin();

    // pcm をキューの末尾に積む。空きスロットが出るまで待つ (最大数百 ms〜数秒)。
    // 戻り値: この PCM の再生開始予定時刻 [ms、esp_timer 基準]。直前のチャンクが
    // まだ鳴っていればその終了時刻、途切れていれば今。
    // cancelled が true を返したら (待っている間も、再生を始める直前も確認する)
    // 積まずに nullopt を返す。
    std::optional<std::uint32_t> enqueue(std::vector<std::int16_t>&& pcm, std::uint32_t sample_rate,
                                         const std::function<bool()>& cancelled = {});

    // 積んだ音の再生が終わる予定時刻 [ms]。何も積んでいなければ 0。
    std::uint32_t end_ms() const { return end_ms_; }

    // 再生予定時刻を過ぎたか (積んだ音を全て鳴らし終えたか)。何も積んでいなければ true。
    bool finished() const;

    // 再生を止める (バッファは次の begin() / release() まで保持: スピーカー タスクが
    // 停止直後に読み終えるまで余裕を持たせるため)。別のタスクから呼んでよい。
    // `before` はミューテックスを持った状態で、スピーカーを止める前に呼ばれる
    // (取り消しフラグを立てるのに使う: enqueue() の cancelled と同時に成り立つ)。
    void stop(const std::function<void()>& before = {});

    // 保持しているバッファを全て解放する。再生が止まっていること。
    void release();

private:
    int channel_;
    mutable std::mutex mtx_;                      // bufs_ / end_ms_ と、再生開始 / 停止の直列化
    std::deque<std::vector<std::int16_t>> bufs_;  // 古い順。末尾が最後に積んだもの
    std::uint32_t end_ms_ = 0;
};

// esp_timer 基準の現在時刻 [ms] (32 ビットで折り返す。差は int32 で取ること)。
std::uint32_t now_ms();

} // namespace stackchan::app
