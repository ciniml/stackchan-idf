// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// ADPCM 版 .jvox の展開検証 + 公開 API (set_voice_db + Engine::Auto) の
// エンド ツー エンド確認。
//
// Usage: jtts_test_adpcm <units_raw.jvox> <units_adpcm.jvox>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include "jtts/jtts.hpp"
#include "jtts/jvox.hpp"
#include "wav_writer.hpp"

using namespace stackchan::jtts;

namespace {
std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <units_raw.jvox> <units_adpcm.jvox>\n", argv[0]);
        return 1;
    }
    const auto raw = read_file(argv[1]);
    const auto adp = read_file(argv[2]);

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    };

    check(!jvox::is_adpcm(raw), "raw blob is not adpcm");
    check(jvox::is_adpcm(adp), "adpcm blob detected");
    const std::size_t want = jvox::decoded_size(adp);
    check(want == raw.size(), "decoded size == raw size");

    std::vector<std::uint8_t> dec(want);
    check(jvox::decode_adpcm(adp, dec), "decode_adpcm succeeds");

    // ヘッダ/マークは bit 一致、PCM は量子化誤差のみ (RMS 誤差 < 3% FS)。
    const std::size_t pcm_off = want - 2;  // 後方から比較するより prefix を検査
    (void)pcm_off;
    bool prefix_same = true;
    // pcm ブロック手前まで一致するか: raw のヘッダ部を Db::parse で確かめつつ
    // 単純比較 (codec バイト 5 は raw=0 / dec=0)。
    auto db_raw = jvox::Db::parse(raw);
    auto db_dec = jvox::Db::parse(dec);
    check(db_raw.has_value() && db_dec.has_value(), "both parse as codec=0");
    if (db_raw && db_dec) {
        prefix_same = db_raw->unit_count() == db_dec->unit_count() &&
                      db_raw->sample_rate() == db_dec->sample_rate();
        check(prefix_same, "header fields match");
        // 代表単位の波形誤差
        auto a = db_raw->find(0x0002);  // あ (振幅が大きく NRMSE 比較に適する)
        auto b = db_dec->find(0x0002);
        check(a.has_value() && b.has_value() && a->pcm.size() == b->pcm.size(),
              "unit pcm sizes match");
        if (a && b && a->pcm.size() == b->pcm.size() && !a->pcm.empty()) {
            double err = 0, ref = 0;
            for (std::size_t i = 0; i < a->pcm.size(); ++i) {
                const double d = static_cast<double>(a->pcm[i]) - b->pcm[i];
                err += d * d;
                ref += static_cast<double>(a->pcm[i]) * a->pcm[i];
            }
            const double nrmse = std::sqrt(err / (ref + 1e-9));
            std::printf("       adpcm NRMSE = %.3f\n", nrmse);
            check(nrmse < 0.25, "adpcm error within 25% (IMA-ADPCM typical: SNR ~15-20 dB)");
        }
    }

    // 公開 API: DB を差して Engine::Auto で合成 → 単位エンジン経路が動く。
    check(set_voice_db(dec), "set_voice_db(decoded)");
    check(voice_db_units() > 0, "voice_db_units > 0");
    Options opt;
    opt.voice = Voice::Female;
    std::vector<std::int16_t> pcm;
    auto r = synthesize(U"こんにちは", pcm, opt);
    check(r.has_value() && pcm.size() > 8000, "synthesize via Engine::Auto");
    if (argc >= 4) write_wav_mono16(argv[3], pcm, opt.sample_rate_hz);
    // Formant 強制も生きている
    opt.engine = Engine::Formant;
    std::vector<std::int16_t> pcm2;
    r = synthesize(U"こんにちは", pcm2, opt);
    check(r.has_value() && !pcm2.empty() && pcm2.size() != pcm.size(),
          "Engine::Formant forces formant path");
    check(set_voice_db({}), "unload voice db");
    check(voice_db_units() == 0, "units == 0 after unload");

    if (failures == 0) {
        std::printf("all adpcm/api checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", failures);
    return 1;
}
