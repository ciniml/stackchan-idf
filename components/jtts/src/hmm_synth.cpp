// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HMM (hts_engine) 合成エンジン。flash mmap / PSRAM 上の .htsvoice イメージを
// ロードし、かな→full-context ラベル (hts_label.cpp) で合成する。
// 48 kHz ボイスはネイティブ合成 → FIR 1/3 デシメーションで 16 kHz 化。
//
// CONFIG_JTTS_ENABLE_HMM が無効なボード (flash に voice を置けない 8 MB 構成)
// ではスタブになり、hts_engine のコードはリンクされない。
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"

#if !defined(ESP_PLATFORM) || defined(CONFIG_JTTS_ENABLE_HMM)
#define JTTS_HMM_AVAILABLE 1
#endif

#ifdef JTTS_HMM_AVAILABLE

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>
#include <string_view>

#include "HTS_engine.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

namespace stackchan::jtts {

namespace {
HTS_Engine g_engine;
bool g_loaded = false;
// エンジンは単一のミュータブル構造なので、合成中の差し替え (HTTP アップロード
// タスク vs 発話タスク) を直列化する。合成は数百 ms 保持するが、差し替えは
// 稀なので単純なミューテックスで足りる。
std::mutex g_engine_mutex;
}  // namespace

bool set_hmm_voice(std::span<const std::uint8_t> htsvoice) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_loaded) {
        HTS_Engine_clear(&g_engine);
        g_loaded = false;
    }
    if (htsvoice.empty()) return true;
    HTS_Engine_initialize(&g_engine);
    const void* datas[1] = {htsvoice.data()};
    const size_t sizes[1] = {htsvoice.size()};
    if (HTS_Engine_load_data(&g_engine, datas, sizes, 1) != TRUE) {
        HTS_Engine_clear(&g_engine);
        return false;
    }
    g_loaded = true;
    return true;
}

bool hmm_voice_loaded() {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    return g_loaded;
}

namespace internal {

namespace {

// 48 kHz → 16 kHz 用 1/3 デシメータ (45-tap Hamming 窓 sinc、fc = 7.2 kHz)
constexpr std::size_t kDecimTaps = 45;

const std::array<float, kDecimTaps>& decim_coeffs() {
    static const std::array<float, kDecimTaps> coeffs = [] {
        std::array<float, kDecimTaps> h{};
        constexpr float fc = 7200.0f / 48000.0f;  // 正規化カットオフ
        constexpr int mid = static_cast<int>(kDecimTaps) / 2;
        float sum = 0.0f;
        for (int i = 0; i < static_cast<int>(kDecimTaps); ++i) {
            const int k = i - mid;
            const float x = 2.0f * std::numbers::pi_v<float> * fc;
            const float sinc = (k == 0) ? 2.0f * fc : std::sin(x * k) / (std::numbers::pi_v<float> * k);
            const float w = 0.54f - 0.46f * std::cos(2.0f * std::numbers::pi_v<float> * i / (kDecimTaps - 1));
            h[i] = sinc * w;
            sum += h[i];
        }
        for (auto& v : h) v /= sum;  // DC ゲイン 1
        return h;
    }();
    return coeffs;
}

// full-context ラベル "p1^p2-p3+p4=p5/A:..." から現在音素 p3 を取り出す。
std::string_view phoneme_of_label(std::string_view label) {
    const std::size_t dash = label.find('-');
    if (dash == std::string_view::npos) return {};
    const std::size_t plus = label.find('+', dash + 1);
    if (plus == std::string_view::npos) return {};
    return label.substr(dash + 1, plus - dash - 1);
}

// 音素名 → 口形。フォルマント エンジン (build_segments) と同じ規則:
//   母音 a i u e o   … その母音の形
//   無声化母音 A I U E O・撥音 N・促音 cl・無音 sil / pau … 閉口
//   両唇音 m b p (拗音含む) … 閉口 (唇を閉じる)
//   その他の子音     … 後続母音の形を先取り (後続が無声化母音なら閉口)
Vowel viseme_of_phoneme(std::string_view ph, std::string_view next) {
    if (ph.size() == 1) {
        switch (ph[0]) {
            case 'a': return Vowel::A;
            case 'i': return Vowel::I;
            case 'u': return Vowel::U;
            case 'e': return Vowel::E;
            case 'o': return Vowel::O;
            case 'A': case 'I': case 'U': case 'E': case 'O': case 'N':
                return Vowel::None;
            default: break;
        }
        if (ph[0] == 'm' || ph[0] == 'b' || ph[0] == 'p') return Vowel::None;
    } else if (ph == "sil" || ph == "pau" || ph == "cl" || ph == "my" || ph == "by" || ph == "py") {
        return Vowel::None;
    }
    // 子音: 後続が有声母音ならその形。
    if (next.size() == 1) {
        switch (next[0]) {
            case 'a': return Vowel::A;
            case 'i': return Vowel::I;
            case 'u': return Vowel::U;
            case 'e': return Vowel::E;
            case 'o': return Vowel::O;
            default: break;
        }
    }
    return Vowel::None;
}

// ---- メモリ予算 / 見積り -------------------------------------------------
//
// hts_engine は 1 回の合成中、フレーム数に比例した作業メモリを確保し続け
// (mean / ivar / wuw / par 行列で ≈ 2 KB / フレーム + 波形 4 B / サンプル)、
// HTS_Engine_refresh まで解放しない。確保に失敗すると hts_engine は復帰できず
// クラッシュするため、合成前に「収まる長さ」を見積もり、超えるなら分割する。
// 実測 (mei16、5 ms フレーム): ≈ 470 KB / 発話秒、≈ 75 KB / 文字。

// テスト用: 0 以外ならこの値 [byte] を予算として使う。
std::size_t g_budget_override = 0;

constexpr float kBytesPerFrame = 2300.0f;  // 実測 ≈ 2.05 KB + ヒープ ヘッダ / 余裕
constexpr float kEdgeSilSec = 0.85f;       // 先頭 + 末尾の sil (各 ≈ 0.32 s) + 1 合成ごとの固定分 (≈ 100〜140 KB)
constexpr float kSecPerMora = 0.15f;       // 等速時の 1 モーラあたり秒 (ポーズ込み、実測 0.138〜0.150)

// 今 1 回の合成に使ってよいバイト数。他タスクの分 (128 KB) を残し、残りの 3/4 を
// 上限とする (見積りの誤差と断片化の余裕)。
std::size_t memory_budget() {
    if (g_budget_override != 0) return g_budget_override;
#if defined(ESP_PLATFORM)
    const std::size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const std::size_t reserve = 128 * 1024;
    return free_psram > reserve ? (free_psram - reserve) / 4 * 3 : 0;
#else
    return SIZE_MAX;  // ホストでは無制限
#endif
}

// 直前までの分割計画が既に安全率を見込んでいるので、2 チャンク目以降の再確認は
// 余裕 (3/4) を掛けずに「実際に足りるか」だけを見る。
std::size_t memory_free_hard() {
    if (g_budget_override != 0) return g_budget_override;
#if defined(ESP_PLATFORM)
    const std::size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return free_psram > 128 * 1024 ? free_psram - 128 * 1024 : 0;
#else
    return SIZE_MAX;
#endif
}

// 予算に収まる 1 チャンクの最大モーラ数 (0 = 1 モーラも収まらない)。
std::size_t max_moras_for_budget(std::size_t budget, std::size_t voice_rate, std::size_t fperiod, float speed) {
    if (budget == SIZE_MAX) return SIZE_MAX;
    const float frames_per_sec = fperiod > 0 ? static_cast<float>(voice_rate) / static_cast<float>(fperiod) : 200.0f;
    const float bytes_per_sec = frames_per_sec * kBytesPerFrame + static_cast<float>(voice_rate) * sizeof(float);
    const float sec = static_cast<float>(budget) / bytes_per_sec - kEdgeSilSec;
    if (sec <= 0.0f) return 0;
    return static_cast<std::size_t>(sec * speed / kSecPerMora);
}

// ---- チャンク境界の無音 ---------------------------------------------------
//
// チャンクごとの合成は前後に sil (≈ 0.32 s) を持つので、そのままつなぐと
// 間延びする。境界では両側の sil を切り詰め、句読点なら元の pau (≈ 0.46 s) と
// 同じ長さ (clause_pause_ms、sanoTTS / フォルマントの句間の無音と共通)、それ以外
// (アクセント句境界 / 強制分割) ならごく短い無音だけ残す。
constexpr std::size_t kSoftKeepFrames = 2;   // その他の境界で片側に残す無音 (≈ 10 ms)

// half_pause_ms: 句読点の境界で片側に残す無音 [ms] (= clause_pause_ms / 2)。
std::size_t boundary_keep_frames(bool pause, float ms_per_frame, float half_pause_ms) {
    if (!pause) return kSoftKeepFrames;
    const auto frames = static_cast<std::size_t>(half_pause_ms / ms_per_frame + 0.5f);
    return frames > kSoftKeepFrames ? frames : kSoftKeepFrames;
}

// 低遅延モードの最初のチャンクの目安モーラ数 (≈ 2 秒の音声 = 合成 ≈ 1.5 秒)。
constexpr std::size_t kStreamFirstMoras = 14;

// 1 チャンクを合成して pcm / spans に出力する。
//   lead_cap / trail_cap: 残す先頭 / 末尾 sil の最大フレーム数 (SIZE_MAX = 全部残す)。
//   need_frames: 継続長が取れないときは失敗にする (複数チャンクでは切り詰めに必須)。
// 失敗時は false (pcm / spans は不定)。
bool synth_chunk(const std::u32string& text, const Options& opt, std::size_t decim, std::size_t voice_rate,
                 std::size_t fperiod, float speed, bool need_frames, std::size_t lead_cap, std::size_t trail_cap,
                 std::vector<std::int16_t>& pcm, std::vector<VisemeSpan>& spans) {
    std::vector<std::string> labels;
    if (!build_hts_labels(text, labels)) return false;

    std::vector<char*> lines;
    lines.reserve(labels.size());
    for (auto& l : labels) lines.push_back(l.data());

    HTS_Engine_set_speed(&g_engine, speed);
    HTS_Engine_add_half_tone(&g_engine, opt.hmm_half_tone);

    const auto t0 = std::chrono::steady_clock::now();
    if (HTS_Engine_synthesize_from_strings(&g_engine, lines.data(), lines.size()) != TRUE) {
        HTS_Engine_refresh(&g_engine);
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();

    const std::size_t nsamples = HTS_Engine_get_nsamples(&g_engine);

    // ラベル (音素) ごとの継続長 [フレーム]。ラベル 1 行 = 1 音素 = nstate 状態で、
    // 総和 × フレーム周期 = 波形長。噛み合わなければ口形も切り詰めも諦める。
    const std::size_t nstate = HTS_Engine_get_nstate(&g_engine);
    std::vector<std::size_t> frames;
    if (nstate > 0 && fperiod > 0 && labels.size() >= 2 &&
        HTS_Engine_get_total_state(&g_engine) == labels.size() * nstate &&
        HTS_Engine_get_total_frame(&g_engine) * fperiod == nsamples) {
        frames.assign(labels.size(), 0);
        for (std::size_t i = 0; i < labels.size(); ++i) {
            for (std::size_t s = 0; s < nstate; ++s) {
                frames[i] += HTS_Engine_get_state_duration(&g_engine, i * nstate + s);
            }
        }
    } else if (need_frames) {
        HTS_Engine_refresh(&g_engine);
        return false;
    }

    // 先頭 / 末尾の sil を切り詰める (波形は [begin, end) だけ出力する)。
    std::size_t begin = 0;
    std::size_t end = nsamples;
    if (!frames.empty()) {
        const std::size_t lead = frames.front();
        const std::size_t trail = frames.back();
        const std::size_t keep_lead = lead < lead_cap ? lead : lead_cap;
        const std::size_t keep_trail = trail < trail_cap ? trail : trail_cap;
        begin = (lead - keep_lead) * fperiod;
        end = nsamples - (trail - keep_trail) * fperiod;
        frames.front() = keep_lead;
        frames.back() = keep_trail;
    }

    pcm.clear();
    const float* speech = g_engine.gss.gspeech;  // per-sample getter は高いので直接参照
    const float gain = opt.gain;
    if (decim == 1) {
        pcm.reserve(end - begin);
        for (std::size_t i = begin; i < end; ++i) {
            float v = speech[i] * gain;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            pcm.push_back(static_cast<std::int16_t>(v));
        }
    } else {
        // 1/3 ポリフェーズ デシメーション (45-tap Hamming sinc、fc=7.2 kHz)。
        // 出力サンプルあたり実質 15 MAC なので合成コストに対して無視できる。
        // フィルタは切り詰め前の全波形を参照するので、境界にもエッジ アーチファクトは出ない。
        const auto& h = decim_coeffs();
        constexpr int mid = static_cast<int>(kDecimTaps) / 2;
        const std::size_t n_begin = begin / decim;
        const std::size_t n_end = end / decim;
        pcm.reserve(n_end - n_begin);
        for (std::size_t n = n_begin; n < n_end; ++n) {
            const long center = static_cast<long>(n) * static_cast<long>(decim);
            long lo = center - mid;
            long hi = center + mid;  // inclusive
            int skip = 0;
            if (lo < 0) {
                skip = static_cast<int>(-lo);
                lo = 0;
            }
            if (hi >= static_cast<long>(nsamples)) hi = static_cast<long>(nsamples) - 1;
            float acc = 0.0f;
            const float* hp = h.data() + skip;
            const float* sp = speech + lo;
            for (long i = lo; i <= hi; ++i) acc += *hp++ * *sp++;
            acc *= gain;
            if (acc > 32767.0f) acc = 32767.0f;
            if (acc < -32768.0f) acc = -32768.0f;
            pcm.push_back(static_cast<std::int16_t>(acc));
        }
    }
    const auto t2 = std::chrono::steady_clock::now();

    spans.clear();
    if (!frames.empty()) {
        const float ms_per_frame = 1000.0f * static_cast<float>(fperiod) / static_cast<float>(voice_rate);
        spans.reserve(labels.size());
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const std::string_view next =
                i + 1 < labels.size() ? phoneme_of_label(labels[i + 1]) : std::string_view{};
            spans.push_back({viseme_of_phoneme(phoneme_of_label(labels[i]), next),
                             static_cast<float>(frames[i]) * ms_per_frame});
        }
    }

#if defined(ESP_PLATFORM)
    ESP_LOGI("jtts-hmm", "synth %u ms + copy %u ms for %u samples @%u Hz",
             static_cast<unsigned>(
                 std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()),
             static_cast<unsigned>(
                 std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()),
             static_cast<unsigned>(nsamples), static_cast<unsigned>(voice_rate));
#else
    (void)t0;
    (void)t1;
    (void)t2;
#endif

    HTS_Engine_refresh(&g_engine);
    return true;
}

}  // namespace

void set_hmm_memory_budget_for_test(std::size_t bytes) { g_budget_override = bytes; }

StreamOutcome render_hmm_stream(std::u32string_view text, const Options& opt, const ChunkFn& emit, bool stream) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (!g_loaded) return StreamOutcome::NoOutput;

    // ボイスはネイティブ レート (48 kHz) のまま合成し、出力レート (16 kHz) へは
    // FIR 1/3 デシメーションで落とす。ボコーダを 16 kHz で直接回す (α 再設定)
    // 近似も試したが、メルケプの周波数軸はどの α でも 48 kHz 分析軸と一致せず
    // フォルマントが下方に歪む (声が暗く低く聞こえる) ため不採用。
    const std::size_t voice_rate = g_engine.ms.sampling_frequency;
    std::size_t decim;
    if (opt.sample_rate_hz == voice_rate) {
        decim = 1;
    } else if (voice_rate == 3 * opt.sample_rate_hz) {
        decim = 3;
    } else {
        return StreamOutcome::NoOutput;  // 対応外レート → フォールバック
    }
    const std::size_t fperiod = HTS_Engine_get_fperiod(&g_engine);

    // mora_ms は「1 モーラの長さ」なので speed は逆比。既定 110 ms = 等速。
    float speed = 110.0f / opt.mora_ms;
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 2.0f) speed = 2.0f;

    // 長い発話はメモリ予算に収まるチャンクに分けて順に合成する。どうしても
    // 収まらない (空きメモリが足りない) ときは諦めて呼び出し側にフォールバック
    // (フォルマント合成) させる — 確保失敗は hts_engine 内でクラッシュになる。
    const std::size_t max_moras = max_moras_for_budget(memory_budget(), voice_rate, fperiod, speed);
    std::vector<HmmChunk> chunks;
    if (!split_hmm_text(text, max_moras, chunks, stream ? kStreamFirstMoras : 0)) {
#if defined(ESP_PLATFORM)
        ESP_LOGW("jtts-hmm", "no memory for HMM synthesis (budget allows %u moras) → fallback",
                 static_cast<unsigned>(max_moras));
#endif
        return StreamOutcome::NoOutput;
    }
    const bool multi = chunks.size() > 1;
    // 切り詰めは fperiod 単位 (デシメーション後も整数サンプル) で行う。
    if (multi && (fperiod == 0 || fperiod % decim != 0)) return StreamOutcome::NoOutput;
#if defined(ESP_PLATFORM)
    if (multi) {
        ESP_LOGI("jtts-hmm", "long text: %u chunks (max %u moras each, PSRAM free %u KB)",
                 static_cast<unsigned>(chunks.size()), static_cast<unsigned>(max_moras),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
#endif

    const float ms_per_frame =
        fperiod > 0 ? 1000.0f * static_cast<float>(fperiod) / static_cast<float>(voice_rate) : 5.0f;
    const float half_pause_ms = 0.5f * clause_pause_ms(opt.mora_ms);
    std::size_t emitted = 0;
    const auto failed = [&] { return emitted > 0 ? StreamOutcome::Aborted : StreamOutcome::NoOutput; };

    std::vector<std::int16_t> pcm;
    std::vector<VisemeSpan> spans;
    for (std::size_t k = 0; k < chunks.size(); ++k) {
        // 2 チャンク目以降は、その間に空きメモリが減っている (再生待ちの PCM など) ので
        // 収まるか確かめ直す。ストリーミングでは分割計画が安全率を見込み済みなので
        // 「実際に足りるか」だけを見る。
        if (k > 0) {
            const std::size_t budget = stream ? memory_free_hard() : memory_budget();
            if (max_moras_for_budget(budget, voice_rate, fperiod, speed) < chunks[k].moras) return failed();
        }
        const bool first = (k == 0);
        const bool last = (k + 1 == chunks.size());
        const std::size_t lead_cap =
            first ? SIZE_MAX : boundary_keep_frames(chunks[k - 1].pause_after, ms_per_frame, half_pause_ms);
        const std::size_t trail_cap =
            last ? SIZE_MAX : boundary_keep_frames(chunks[k].pause_after, ms_per_frame, half_pause_ms);
        if (!synth_chunk(chunks[k].text, opt, decim, voice_rate, fperiod, speed, multi, lead_cap, trail_cap, pcm,
                         spans)) {
            return failed();
        }
        ++emitted;
        if (!emit(std::move(pcm), std::move(spans), chunks[k].text)) return StreamOutcome::Cancelled;
        pcm = {};
        spans = {};
    }
    return StreamOutcome::Ok;
}

}  // namespace internal
}  // namespace stackchan::jtts

#else  // !JTTS_HMM_AVAILABLE — スタブ (hts_engine をリンクしない)

namespace stackchan::jtts {

bool set_hmm_voice(std::span<const std::uint8_t>) { return false; }
bool hmm_voice_loaded() { return false; }

namespace internal {
StreamOutcome render_hmm_stream(std::u32string_view, const Options&, const ChunkFn&, bool) {
    return StreamOutcome::NoOutput;
}
void set_hmm_memory_budget_for_test(std::size_t) {}
}  // namespace internal

}  // namespace stackchan::jtts

#endif  // JTTS_HMM_AVAILABLE
