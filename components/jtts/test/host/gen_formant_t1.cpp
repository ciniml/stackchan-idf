// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// gen_formant_t1: フォルマント合成 tranche 1 (鼻音ゼロ / 破裂音 VOT /
// 摩擦音整形) の検聴用ミニマルペア WAV を generated/ に書き出す。
// engine=Formant を明示 (Auto のフォールバック経路に依存しない)。
//
// Usage: gen_formant_t1 <output-dir>

#include <cstdio>
#include <string>
#include <vector>

#include "jtts/jtts.hpp"
#include "wav_writer.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <output-dir>\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];

    struct Case {
        const char32_t* kana;
        const char* file;
    };
    const Case cases[] = {
        {U"なまえ、まみむめも、んーん", "formant_t1_nasals.wav"},
        {U"かた、がだば、ぱぴぷ、きった", "formant_t1_stops.wav"},
        {U"さしすせそ、しゃしゅしょ、ざじず、まっすぐ", "formant_t1_fric.wav"},
        {U"きょうのてんきははれです", "formant_t1_sentence.wav"},
    };

    stackchan::jtts::Options opt;
    opt.engine = stackchan::jtts::Engine::Formant;

    for (const Case& c : cases) {
        std::vector<std::int16_t> pcm;
        auto r = stackchan::jtts::synthesize(c.kana, pcm, opt);
        if (!r) {
            std::fprintf(stderr, "synthesize failed: %s (%s)\n",
                         stackchan::jtts::to_string(r.error()), c.file);
            return 2;
        }
        const std::string path = dir + "/" + c.file;
        if (!write_wav_mono16(path, pcm, opt.sample_rate_hz)) {
            std::fprintf(stderr, "wav write failed: %s\n", path.c_str());
            return 3;
        }
        std::printf("wrote %s (%zu samples)\n", path.c_str(), pcm.size());
    }
    return 0;
}
