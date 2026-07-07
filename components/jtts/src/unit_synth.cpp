// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// モーラ単位連結 + TD-PSOLA エンジン (案 B)。
// .jvox の単位波形 (実音声 or ブートストラップ合成) を連結し、ピッチマーク
// 同期の overlap-add で目標 F0・時間長に変形する。
//
// 連結品質のための 3 つの仕掛け (単位ごとの独立レンダリングだと境界が
// 「ぼそぼそ」と脈動するのを実測で確認して入れた):
//   1. 合成ピッチ パルスは発話全体で 1 本の連続列 — 単位境界を跨いでも
//      パルス間隔が乱れない (無声区間が長いときだけ位相リセット)。
//   2. グレインは自単位の出力区間に閉じず隣へあふれる — 境界で窓の重なりが
//      切れてエネルギーの谷ができない (自然なクロスフェードになる)。
//   3. グレインごとのエネルギー正規化 — 元発話の語尾減衰や強勢が単位ごと
//      に持ち込まれてモーラ周期の音量脈動になるのを、単位の代表 RMS へ
//      揃えて平らにする (母音ごとの地の音量差は保つ)。
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "internal.hpp"
#include "jtts/jvox.hpp"

namespace stackchan::jtts::internal {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// レンダリング計画 1 エントリ = 1 単位の出力配置。
// テール (最終マーク以降の残差) は出力しない: 中身は元発話の消え際 +
// トリム余白のほぼ無音で、有声部の間に挟まると単位ごとに音量が落ち込む
// (実聴で確認)。時間はぜんぶ有声部に配分する。
struct Placed {
    jvox::UnitView unit;
    float amp = 1.0f;         // 無声化母音などの減衰
    std::size_t out0 = 0;     // 単位出力の先頭 (mix 内)
    std::size_t head_len = 0; // 無声オンセット (verbatim コピー) の長さ
    std::size_t v_out_len = 0;// 有声部の出力長 (PSOLA)
    std::size_t total = 0;    // head+voiced+gap を含む占有長
    float ref_rms = 0.0f;     // グレイン正規化の目標 RMS (単位の代表値)
    // 分析写像の範囲 (マーク添字)。末尾は「実のある」(local RMS ≥ 0.4×ref)
    // 最後のマークまで — 元発話の減衰しきった尻尾へ写像すると、正規化の
    // クランプ (≤4×) を超えて音量の谷になる。
    std::size_t mark_last = 0;
};

// 分析マーク周辺 ±p の生 RMS。グレイン正規化の分子/分母に使う。
float local_rms(std::span<const std::int16_t> pcm, std::size_t center, std::size_t p) {
    const std::size_t lo = (center > p) ? center - p : 0;
    const std::size_t hi = std::min(pcm.size(), center + p + 1);
    if (hi <= lo) return 0.0f;
    float acc = 0.0f;
    for (std::size_t i = lo; i < hi; ++i) {
        const float v = static_cast<float>(pcm[i]) / 32768.0f;
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<float>(hi - lo));
}

// 単位の代表 RMS: マーク列の前半 70% (語尾減衰を含めない) の中央値。
float unit_ref_rms(const jvox::UnitView& u) {
    if (u.marks.size() < 3) return 0.0f;
    const std::size_t n = std::max<std::size_t>(1, u.marks.size() * 7 / 10);
    std::vector<float> vals;
    vals.reserve(n);
    for (std::size_t j = 1; j + 1 < u.marks.size() && vals.size() < n; ++j) {
        const std::size_t p = u.marks[j] - u.marks[j - 1];
        vals.push_back(local_rms(u.pcm, u.marks[j], p));
    }
    if (vals.empty()) return 0.0f;
    std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
    return vals[vals.size() / 2];
}

// verbatim コピー (amp 適用、端 3 ms のフェード付き) を mix に加算する。
void copy_with_fade(std::span<const std::int16_t> pcm, std::size_t src0, std::size_t n,
                    float amp, std::vector<float>& mix, std::size_t dst0, float fs) {
    n = std::min({n, pcm.size() - src0, mix.size() - dst0});
    if (n == 0) return;
    const std::size_t fade = std::min<std::size_t>(n / 2, static_cast<std::size_t>(fs * 0.003f));
    for (std::size_t i = 0; i < n; ++i) {
        float w = 1.0f;
        if (fade > 0) {
            if (i < fade) w = static_cast<float>(i) / static_cast<float>(fade);
            const std::size_t from_end = n - 1 - i;
            if (from_end < fade) {
                w = std::min(w, static_cast<float>(from_end) / static_cast<float>(fade));
            }
        }
        mix[dst0 + i] += amp * w * static_cast<float>(pcm[src0 + i]) / 32768.0f;
    }
}

}  // namespace

bool render_units(std::span<const Mora> moras, const jvox::Db& db,
                  std::vector<std::int16_t>& out, const Options& opt) {
    const float fs = static_cast<float>(opt.sample_rate_hz);
    auto ms_to_samples = [&](float ms) {
        return static_cast<std::size_t>(std::lround(ms * 0.001f * fs));
    };

    // --- 計画: 必要な単位が全部あるか先に確かめつつ出力配置を決める ---
    std::vector<Placed> plan;
    plan.reserve(moras.size());
    std::size_t cursor = 0;
    for (const Mora& m : moras) {
        switch (m.kind) {
            case MoraKind::CV:
            case MoraKind::MoraicN: {
                const std::uint16_t key =
                    (m.kind == MoraKind::MoraicN) ? jvox::kMoraicNKey : jvox::unit_key(m);
                auto u = db.find(key);
                if (!u) return false;  // 欠け → フォルマント エンジンへ
                Placed e;
                e.unit = *u;
                float dur_ms = opt.mora_ms;
                if (m.kind == MoraKind::CV && m.devoiced) {
                    e.amp = 0.5f;
                    dur_ms *= 0.8f;
                }
                e.out0 = cursor;
                const std::size_t target = ms_to_samples(dur_ms);
                if (e.unit.marks.size() >= 3) {
                    const std::size_t v_in0 = e.unit.marks.front();
                    e.head_len = std::min<std::size_t>(v_in0, target);
                    e.v_out_len = target - e.head_len;
                    e.ref_rms = unit_ref_rms(e.unit);
                    // 写像の末尾: 実のある最後のマーク。
                    e.mark_last = e.unit.marks.size() - 1;
                    while (e.mark_last > 2) {
                        const std::size_t j = e.mark_last;
                        const std::size_t p = e.unit.marks[j] - e.unit.marks[j - 1];
                        if (local_rms(e.unit.pcm, e.unit.marks[j], p) >= 0.4f * e.ref_rms) {
                            break;
                        }
                        --e.mark_last;
                    }
                } else {
                    // ピッチマークなし (無声単位): 等速コピーのみ。
                    e.head_len = std::min(e.unit.pcm.size(), target);
                }
                e.total = target;
                plan.push_back(e);
                cursor += target;
                break;
            }
            case MoraKind::Sokuon:
                cursor += ms_to_samples(70.0f);  // 閉鎖の無音
                break;
            case MoraKind::Chouon:
                if (!plan.empty()) {
                    const std::size_t ext = ms_to_samples(opt.mora_ms);
                    plan.back().v_out_len += ext;
                    plan.back().total += ext;
                    cursor += ext;
                }
                break;
        }
    }
    if (plan.empty()) return false;
    const std::size_t total_len = cursor;

    // 韻律: フォルマント エンジンと同じ句輪郭を目標 F0 に使う。
    const ProsodyCurve curve(static_cast<float>(total_len) * 1000.0f / fs);
    const float base_f0 = opt.f0_hz;
    auto f0_at = [&](std::size_t s) {
        const float t_ms = static_cast<float>(s) * 1000.0f / fs;
        float f0 = base_f0 * curve.at(t_ms);
        return (f0 < 40.0f) ? 40.0f : f0;
    };

    std::vector<float> mix(total_len, 0.0f);

    // --- 無声部 (head) の verbatim コピー ---
    for (const Placed& e : plan) {
        copy_with_fade(e.unit.pcm, 0, e.head_len, e.amp, mix, e.out0, fs);
    }

    // --- 有声部: 発話全体で連続な合成パルス列による PSOLA ---
    float s = -1.0f;  // 次の合成マーク位置 (負 = 未開始)
    for (const Placed& e : plan) {
        if (e.v_out_len == 0 || e.unit.marks.size() < 3) continue;
        const float v0 = static_cast<float>(e.out0 + e.head_len);
        const float v1 = v0 + static_cast<float>(e.v_out_len);

        // パルス位相の継続: 前の有声部からの間隔が 2 周期 + 20 ms を超えたら
        // リセット (子音・無音を挟んだら位相合わせは不要)。それ以下なら
        // 周期を刻んだまま次の有声部に入る — 境界でパルス間隔が乱れない。
        const float period0 = fs / f0_at(static_cast<std::size_t>(v0));
        if (s < 0.0f || v0 - s > 2.0f * period0 + fs * 0.020f) {
            s = v0;
        } else {
            while (s < v0) s += fs / f0_at(static_cast<std::size_t>(s));
        }

        const float v_in0 = static_cast<float>(e.unit.marks.front());
        const float v_in_len =
            static_cast<float>(e.unit.marks[e.mark_last] - e.unit.marks.front());

        // グレイン正規化ゲインの平滑化状態。毎グレイン独立にゲインが跳ねると
        // ピッチ周期の振幅変調 (ざらついたノイズ) になる。
        float g_smooth = -1.0f;

        while (s < v1) {
            const std::size_t out_pos = static_cast<std::size_t>(s);
            const float f0 = f0_at(out_pos);
            const float syn_period = fs / f0;

            // 分析位置: 有声部の進捗を入力へ線形写像し、最寄りのマークを選ぶ。
            const float uu =
                v_in0 + ((s - v0) / static_cast<float>(e.v_out_len)) * v_in_len;
            std::size_t j = 1;
            while (j + 1 <= e.mark_last && static_cast<float>(e.unit.marks[j]) < uu) {
                ++j;
            }
            const std::size_t m = e.unit.marks[j];
            const std::size_t p_prev = e.unit.marks[j] - e.unit.marks[j - 1];
            const std::size_t p_next =
                (j + 1 < e.unit.marks.size()) ? e.unit.marks[j + 1] - e.unit.marks[j] : p_prev;
            const std::size_t p_a = std::max<std::size_t>(8, std::min(p_prev, p_next));

            // グレイン エネルギー正規化: このグレインの生 RMS を単位の代表
            // RMS に揃える。元発話の語尾減衰・強勢の持ち込みを平らにする。
            // データ側 (pack_jvox) で単位間レベルは揃っている前提なので
            // 補正は控えめ (±2 倍) + 平滑化 (跳ねるとピッチ周期の変調
            // ノイズになる)。
            float gain = e.amp;
            if (e.ref_rms > 0.0f) {
                const float g_rms = local_rms(e.unit.pcm, m, p_a);
                if (g_rms > 1e-5f) {
                    const float target = std::clamp(e.ref_rms / g_rms, 0.5f, 2.0f);
                    g_smooth = (g_smooth < 0.0f) ? target : 0.6f * g_smooth + 0.4f * target;
                    gain *= g_smooth;
                }
            }
            // ピッチを上げる (合成間隔 < 分析周期) と窓の重なりが増えて音量が
            // 上がるぶんの補正。
            gain *= std::clamp(syn_period / static_cast<float>(p_a), 0.5f, 2.0f);

            // 2 周期 Hann 窓グレイン。自単位の区間に閉じず mix 全体に置く —
            // 境界で窓の重なりが切れてエネルギーの谷ができないように。
            for (std::size_t k = 0; k < 2 * p_a + 1; ++k) {
                const std::ptrdiff_t src =
                    static_cast<std::ptrdiff_t>(m + k) - static_cast<std::ptrdiff_t>(p_a);
                if (src < 0 || src >= static_cast<std::ptrdiff_t>(e.unit.pcm.size())) continue;
                const std::ptrdiff_t dst = static_cast<std::ptrdiff_t>(out_pos + k) -
                                           static_cast<std::ptrdiff_t>(p_a);
                if (dst < 0 || dst >= static_cast<std::ptrdiff_t>(total_len)) continue;
                const float w =
                    0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(k) /
                                           static_cast<float>(2 * p_a));
                mix[static_cast<std::size_t>(dst)] +=
                    gain * w *
                    static_cast<float>(e.unit.pcm[static_cast<std::size_t>(src)]) / 32768.0f;
            }
            s += syn_period;
        }
    }

    // 発話全体の端のフェード (2 ms) — 先頭/末尾の DC ステップ除去。
    const std::size_t fade = std::min<std::size_t>(mix.size() / 4, static_cast<std::size_t>(fs * 0.002f));
    for (std::size_t i = 0; i < fade; ++i) {
        const float w = static_cast<float>(i) / static_cast<float>(fade);
        mix[i] *= w;
        mix[mix.size() - 1 - i] *= w;
    }

    out.reserve(out.size() + mix.size());
    for (float v : mix) {
        float y = std::clamp(v * opt.gain, -1.0f, 1.0f);
        out.push_back(static_cast<std::int16_t>(y * 32760.0f));
    }
    return true;
}

}  // namespace stackchan::jtts::internal
