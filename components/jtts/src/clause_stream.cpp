// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 句ごとの合成の制御 (sanoTTS / フォルマント / 単位連結で共通、エンジンには依存しない)。
// 1 句の合成は関数で受け取るので、重み (非 MIT の blob) や音声 DB が無くてもホストで
// テストできる。設計は internal.hpp の「句ごとの合成」を参照。
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "internal.hpp"

namespace stackchan::jtts::internal {

SilenceTrim trim_silence(std::vector<std::int16_t>& pcm, std::uint32_t rate_hz, bool lead, bool trail,
                         std::uint32_t keep_ms) {
    SilenceTrim removed;
    if (pcm.empty() || (!lead && !trail)) return removed;
    int peak = 0;
    for (std::int16_t v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
    const int thr = std::max(48, peak / 100);
    std::size_t first = 0;
    while (first < pcm.size() && std::abs(static_cast<int>(pcm[first])) < thr) ++first;
    if (first == pcm.size()) return removed;  // 全体が無音: 触らない
    std::size_t last = pcm.size();            // 最後の音の次
    while (last > first && std::abs(static_cast<int>(pcm[last - 1])) < thr) --last;

    const std::size_t keep = static_cast<std::size_t>(keep_ms) * rate_hz / 1000u;
    const std::size_t begin = lead ? (first > keep ? first - keep : 0) : 0;
    const std::size_t end = trail ? std::min(pcm.size(), last + keep) : pcm.size();
    removed.back = pcm.size() - end;
    removed.front = begin;
    pcm.erase(pcm.begin() + static_cast<std::ptrdiff_t>(end), pcm.end());
    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(begin));
    return removed;
}

void crop_spans(std::vector<VisemeSpan>& spans, float drop_front_ms, float keep_ms) {
    std::vector<VisemeSpan> out;
    float t = 0.0f;                       // 元の時間軸での現在位置
    const float keep_end = drop_front_ms + keep_ms;
    for (const VisemeSpan& s : spans) {
        const float b = std::max(t, drop_front_ms);
        const float e = std::min(t + s.duration_ms, keep_end);
        if (e > b) out.push_back({s.vowel, e - b});
        t += s.duration_ms;
    }
    spans = std::move(out);
}

StreamOutcome stream_clauses(std::u32string_view text, const Options& opt, const ClauseSynth& synth_one,
                             const ClauseChunkFn& emit, bool trim_edges) {
    std::vector<HmmChunk> clauses;
    if (!split_clauses(text, clauses)) return StreamOutcome::NoOutput;

    const float pause_ms = clause_pause_ms(opt.mora_ms);

    std::size_t emitted = 0;
    for (std::size_t k = 0; k < clauses.size(); ++k) {
        std::vector<std::int16_t> pcm;
        std::vector<VisemeSpan> spans;
        std::uint32_t rate = 0;
        const ClauseResult r = synth_one(clauses[k].text, pcm, rate, spans);
        if (r == ClauseResult::Skip) continue;
        if (r == ClauseResult::Fail || pcm.empty() || rate == 0) {
            return emitted > 0 ? StreamOutcome::Aborted : StreamOutcome::NoOutput;
        }
        const bool has_prev = emitted > 0;
        const bool has_next = k + 1 < clauses.size();
        // 内側の境界: モデルが付けた前後の無音は切り詰めて、こちらの無音に置き換える
        // (trim_edges のとき)。発話の先頭 / 末尾は切り詰めない。口形の区間も同じだけ切り詰める。
        const SilenceTrim cut = trim_edges ? trim_silence(pcm, rate, /*lead=*/has_prev, /*trail=*/has_next)
                                           : SilenceTrim{};
        const float ms_per_sample = 1000.0f / static_cast<float>(rate);
        if (!spans.empty() && (cut.front != 0 || cut.back != 0)) {
            crop_spans(spans, static_cast<float>(cut.front) * ms_per_sample,
                       static_cast<float>(pcm.size()) * ms_per_sample);
        }
        if (has_next) {
            const auto pause_samples = static_cast<std::size_t>(pause_ms * static_cast<float>(rate) / 1000.0f);
            pcm.resize(pcm.size() + pause_samples, 0);
            if (!spans.empty()) spans.push_back({Vowel::None, static_cast<float>(pause_samples) * ms_per_sample});
        }
        ++emitted;
        if (!emit(std::move(pcm), rate, std::move(spans), clauses[k].text)) return StreamOutcome::Cancelled;
    }
    return emitted > 0 ? StreamOutcome::Ok : StreamOutcome::NoOutput;
}

}  // namespace stackchan::jtts::internal
