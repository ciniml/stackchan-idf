// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS-jp 合成エンジン。重み blob (flash mmap / PSRAM) を上流コア
// (components/saanotts/upstream) に直接参照させ、ストリーミング API で合成する。
// 出力は 22.05 kHz 固定 (再生側がレートを合わせる)。
//
// 作業領域 (arena、176 KB) は初回に PSRAM から確保する (無ければ内部 DRAM)。
// 上流の公式ファームは内部 DRAM の静的配列だが、Stack-chan の内部 RAM には
// 余裕が無い。PSRAM 上の arena での速度は Phase 1 で実測する。
//
// CONFIG_JTTS_ENABLE_SANOTTS が無効なボードではスタブになり、コアはリンクされない。
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"

#if !defined(ESP_PLATFORM) || defined(CONFIG_JTTS_ENABLE_SANOTTS)
#define JTTS_SANO_AVAILABLE 1
#endif

#ifdef JTTS_SANO_AVAILABLE

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>

extern "C" {
#include "g2p.h"
#include "saanotts.h"
#include "saanotts_stream.h"
}

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#define SANO_LOGI(...) ESP_LOGI("jtts-sano", __VA_ARGS__)
#define SANO_LOGW(...) ESP_LOGW("jtts-sano", __VA_ARGS__)
#else
#include <cstdio>
#define SANO_LOGI(...) do { std::fprintf(stderr, "[jtts-sano] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define SANO_LOGW(...) SANO_LOGI(__VA_ARGS__)
#endif

namespace stackchan::jtts {

namespace {

// 上流 esp32/main/main.c の SAAN_ARENA_BYTES と同じ。W8A8 で 350 ids まで。
constexpr std::size_t kArenaBytes = 176 * 1024;

std::mutex g_mutex;
saan_weights g_weights{};
bool g_loaded = false;
std::uint8_t* g_arena = nullptr;

bool ensure_arena() {
    if (g_arena != nullptr) return true;
#if defined(ESP_PLATFORM)
    g_arena = static_cast<std::uint8_t*>(
        heap_caps_aligned_alloc(16, kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_arena != nullptr) {
        SANO_LOGI("arena %u B in PSRAM", static_cast<unsigned>(kArenaBytes));
        return true;
    }
    g_arena = static_cast<std::uint8_t*>(
        heap_caps_aligned_alloc(16, kArenaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (g_arena != nullptr) {
        SANO_LOGW("arena %u B in internal DRAM (no PSRAM)", static_cast<unsigned>(kArenaBytes));
        return true;
    }
    SANO_LOGW("arena %u B: out of memory", static_cast<unsigned>(kArenaBytes));
    return false;
#else
    g_arena = static_cast<std::uint8_t*>(std::aligned_alloc(16, kArenaBytes));
    return g_arena != nullptr;
#endif
}

}  // namespace

bool set_sano_weights(std::span<const std::uint8_t> blob) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = false;
    if (blob.empty()) return true;
    if ((reinterpret_cast<std::uintptr_t>(blob.data()) & 15u) != 0) {
        SANO_LOGW("weights blob is not 16-byte aligned (%p)", static_cast<const void*>(blob.data()));
        return false;
    }
    const saan_status s = saan_weights_open(&g_weights, blob.data(), blob.size());
    if (s != SAAN_OK) {
        SANO_LOGW("saan_weights_open: %s", saan_strerror(s));
        return false;
    }
    g_loaded = true;
    SANO_LOGI("weights v%u, %u tensors, %u B", static_cast<unsigned>(g_weights.version),
              static_cast<unsigned>(g_weights.n_tensors), static_cast<unsigned>(g_weights.size));
    return true;
}

bool sano_weights_loaded() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_loaded;
}

namespace internal {

bool render_sano(std::u32string_view text, std::vector<std::int16_t>& out, const Options& opt,
                 std::uint32_t& out_rate_hz) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_loaded) return false;

    std::string ir;
    if (!build_sano_ir(text, ir)) return false;

    const std::int32_t cap = saan_g2p_capacity(ir.size());
    std::vector<std::int32_t> ids(static_cast<std::size_t>(cap));
    std::int32_t n_ids = 0;
    saan_g2p_info info{};
    const saan_g2p_status gs = saan_g2p(ir.data(), ir.size(), ids.data(), cap, &n_ids, &info);
    if (gs != SAAN_G2P_OK) {
        SANO_LOGW("g2p: %s at byte %d (ir='%s')", saan_g2p_strerror(gs), static_cast<int>(info.err_byte),
                  ir.c_str());
        return false;
    }
    if (!ensure_arena()) return false;
    // saan_stream_arena_needed() は上限式で実使用 (arena.peak) より大きく出るので、
    // 事前判定には使わず init の SAAN_ERR_ARENA に任せる (上流の雛形と同じ)。

    // s_v は duration のスケール (大きいほど遅い)。mora_ms 110 = 等速。
    float speed = opt.mora_ms / 110.0f;
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 2.0f) speed = 2.0f;
    const float s_v = SAAN_S_V * speed;

    saan_arena arena{};
    saan_arena_init(&arena, g_arena, kArenaBytes);
    saan_stream st{};
    const auto t0 = std::chrono::steady_clock::now();
    saan_status s = saan_stream_init(&st, &g_weights, &arena, ids.data(), n_ids, s_v);
    if (s != SAAN_OK) {
        SANO_LOGW("stream_init: %s (%d ids, arena %u B, needed<=%u B)", saan_strerror(s), static_cast<int>(n_ids),
                  static_cast<unsigned>(kArenaBytes), static_cast<unsigned>(saan_stream_arena_needed(n_ids)));
        return false;
    }

    std::vector<float> chunk(static_cast<std::size_t>(SAAN_CHUNK) * SAAN_HOP);
    const float gain = opt.gain * 32767.0f;
    for (;;) {
        std::int32_t n_out = 0;
        s = saan_stream_pull(&st, chunk.data(), &n_out);
        if (s != SAAN_OK) {
            SANO_LOGW("stream_pull: %s", saan_strerror(s));
            out.clear();
            return false;
        }
        if (n_out == 0) break;
        const std::size_t n = static_cast<std::size_t>(n_out) * SAAN_HOP;
        for (std::size_t i = 0; i < n; ++i) {
            float v = chunk[i] * gain;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out.push_back(static_cast<std::int16_t>(std::lrintf(v)));
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    const float audio_ms = static_cast<float>(out.size()) * 1000.0f / static_cast<float>(SAAN_SR);
    SANO_LOGI("%d ids -> %u samples (%.0f ms) in %lld ms, RTF %.2f, arena peak %u B", static_cast<int>(n_ids),
              static_cast<unsigned>(out.size()), audio_ms, static_cast<long long>(ms),
              audio_ms > 0 ? static_cast<float>(ms) / audio_ms : 0.0f, static_cast<unsigned>(arena.peak));
    out_rate_hz = SAAN_SR;
    return true;
}

}  // namespace internal

}  // namespace stackchan::jtts

#else  // !JTTS_SANO_AVAILABLE

namespace stackchan::jtts {
bool set_sano_weights(std::span<const std::uint8_t>) { return false; }
bool sano_weights_loaded() { return false; }
namespace internal {
bool render_sano(std::u32string_view, std::vector<std::int16_t>&, const Options&, std::uint32_t&) {
    return false;
}
}  // namespace internal
}  // namespace stackchan::jtts

#endif
