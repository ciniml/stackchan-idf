// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "sano_bench.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <jtts/jtts.hpp>

#include "sano_weights.hpp"

namespace stackchan::app::sano_bench {

namespace {
constexpr const char* kTag = "sano-bench";

void log_heap(const char* when) {
    ESP_LOGI(kTag, "%s: INT free=%u largest=%u | PSRAM free=%u", when,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
}  // namespace

void run() {
    log_heap("before weights");
    if (!sano_weights::init()) {
        ESP_LOGW(kTag, "no sanoTTS weights — bench skipped");
        return;
    }
    log_heap("after weights");
    const char32_t* phrases[] = {
        U"きょ'お/わ'よい/て'んき/です'ね",
        U"わたしはすたっくちゃんです",
        U"ながいぶんしょうのてすとです。わたしはすたっくちゃんといいます。きょうはとてもいいてんきですね。",
    };
    jtts::Options opt;
    opt.engine = jtts::Engine::Sano;
    std::vector<std::int16_t> pcm;
    for (const char32_t* p : phrases) {
        pcm.clear();
        auto r = jtts::synthesize_ex(std::u32string_view{p}, pcm, opt);
        if (!r) {
            ESP_LOGW(kTag, "synthesize failed: %s", jtts::to_string(r.error()));
        }
    }
    log_heap("after bench");
}

}  // namespace stackchan::app::sano_bench
