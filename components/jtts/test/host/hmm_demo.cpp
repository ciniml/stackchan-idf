// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HMM エンジンのホスト デモ / 検証 CLI。
//   jtts_hmm_demo labels <かな>                     … ラベルを stdout にダンプ
//   jtts_hmm_demo synth <voice.htsvoice> <かな> <out.wav> [half_tone] [mora_ms]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"
#include "wav_writer.hpp"

using namespace stackchan::jtts;

namespace {

std::u32string to_u32(const char* utf8) {
    std::u32string out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8);
    while (*p != 0) {
        char32_t cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p >> 5) == 0x6) {
            cp = (*p++ & 0x1F) << 6;
            cp |= (*p++ & 0x3F);
        } else if ((*p >> 4) == 0xE) {
            cp = (*p++ & 0x0F) << 12;
            cp |= (*p++ & 0x3F) << 6;
            cp |= (*p++ & 0x3F);
        } else {
            cp = (*p++ & 0x07) << 18;
            cp |= (*p++ & 0x3F) << 12;
            cp |= (*p++ & 0x3F) << 6;
            cp |= (*p++ & 0x3F);
        }
        out.push_back(cp);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "labels") == 0) {
        std::vector<std::string> labels;
        if (!internal::build_hts_labels(to_u32(argv[2]), labels)) {
            std::fprintf(stderr, "label build failed\n");
            return 1;
        }
        for (const auto& l : labels) std::printf("%s\n", l.c_str());
        return 0;
    }
    if (argc >= 5 && std::strcmp(argv[1], "synth") == 0) {
        std::ifstream f(argv[2], std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", argv[2]);
            return 1;
        }
        std::vector<std::uint8_t> voice((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
        if (!set_hmm_voice(voice)) {
            std::fprintf(stderr, "voice load failed\n");
            return 1;
        }
        Options opt;
        opt.engine = Engine::Hmm;
        opt.gain = 1.0f;
        if (argc >= 6) opt.hmm_half_tone = std::strtof(argv[5], nullptr);
        if (argc >= 7) opt.mora_ms = std::strtof(argv[6], nullptr);
        std::vector<std::int16_t> pcm;
        auto r = synthesize(to_u32(argv[3]), pcm, opt);
        if (!r) {
            std::fprintf(stderr, "synthesize failed: %s\n", to_string(r.error()));
            return 1;
        }
        if (!write_wav_mono16(argv[4], pcm, opt.sample_rate_hz)) {
            std::fprintf(stderr, "wav write failed\n");
            return 1;
        }
        std::printf("%s: %zu samples (%.2f s)\n", argv[4], pcm.size(),
                    static_cast<double>(pcm.size()) / opt.sample_rate_hz);
        return 0;
    }
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s labels <kana>\n"
                 "  %s synth <voice.htsvoice> <kana> <out.wav> [half_tone] [mora_ms]\n",
                 argv[0], argv[0]);
    return 2;
}
