// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// jtts_sano_demo: sanoTTS-jp の重み blob とかなを受け取り WAV に書く。
//   jtts_sano_demo saanotts-jp-v4-int8.bin "こんにちは" out.wav [mora_ms]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "jtts/jtts.hpp"
#include "wav_writer.hpp"

namespace {
std::u32string utf8_to_u32(const char* s) {
    std::u32string out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        char32_t c;
        int n;
        if (*p < 0x80) { c = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F; n = 2; }
        else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F; n = 3; }
        else { c = *p & 0x07; n = 4; }
        for (int i = 1; i < n && p[i]; ++i) c = (c << 6) | (p[i] & 0x3F);
        out.push_back(c);
        p += n;
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s weights.bin \"かな\" out.wav [mora_ms]\n", argv[0]);
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
    if (raw.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    // コアは 16 バイト境界を要求する。
    void* buf = std::aligned_alloc(16, (raw.size() + 15) & ~std::size_t{15});
    std::memcpy(buf, raw.data(), raw.size());
    if (!stackchan::jtts::set_sano_weights({static_cast<const std::uint8_t*>(buf), raw.size()})) {
        std::fprintf(stderr, "set_sano_weights failed\n");
        return 1;
    }
    stackchan::jtts::Options opt;
    opt.engine = stackchan::jtts::Engine::Sano;
    opt.gain = 0.9f;
    if (argc >= 5) opt.mora_ms = static_cast<float>(std::atof(argv[4]));
    std::vector<std::int16_t> pcm;
    auto r = stackchan::jtts::synthesize_ex(utf8_to_u32(argv[2]), pcm, opt);
    if (!r) { std::fprintf(stderr, "synthesize_ex failed: %s\n", stackchan::jtts::to_string(r.error())); return 1; }
    if (!write_wav_mono16(argv[3], pcm, *r)) { std::fprintf(stderr, "wav write failed\n"); return 1; }
    std::printf("%s: %zu samples @ %u Hz (%.2f s)\n", argv[3], pcm.size(), *r,
                static_cast<double>(pcm.size()) / *r);
    return 0;
}
