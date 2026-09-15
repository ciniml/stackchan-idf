// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// かな (jtts 記法) → sanoTTS-jp 中間表現の写像テスト。
#include <cstdio>
#include <string>
#include <string_view>

#include "internal.hpp"

namespace {
int g_fail = 0;
void check(std::u32string_view in, std::string_view expect, const char* label) {
    std::string ir;
    const bool ok = stackchan::jtts::internal::build_sano_ir(in, ir);
    const bool expect_ok = !expect.empty();
    if (ok != expect_ok || ir != expect) {
        std::printf("FAIL %s: got %s '%s' expected '%s'\n", label, ok ? "ok" : "false", ir.c_str(),
                    std::string(expect).c_str());
        ++g_fail;
    }
}
}  // namespace

int main() {
    check(U"こんにちは", "こ[んにちは", "heiban 5 moras");
    check(U"あ", "あ", "single mora");
    check(U"あ'め", "あ]め", "atamadaka (rain)");
    check(U"あめ'", "あ[め]", "odaka (candy)");
    check(U"はし'", "は[し]", "odaka");
    check(U"こ'ころ", "こ]ころ", "atamadaka 3");
    check(U"たま'ご", "た[ま]ご", "nakadaka");
    check(U"きょう、あした。", "きょ[う_あ[した", "pause + sentence end dropped");
    check(U"テスト", "て[すと", "katakana");
    check(U"きって", "き[って", "sokuon");
    check(U"ラーメン", "ら[ーめん", "long vowel + N");
    check(U"きょ'お/わ'よい", "きょ]おわ]よい", "phrase boundary (no # in v1)");
    check(U"わたし'は/がくせいです", "わ[たし]はが[くせいです", "nakadaka + heiban");
    check(U"、、", "", "no moras");
    if (g_fail == 0) {
        std::printf("sano_ir: all tests passed\n");
        return 0;
    }
    std::printf("sano_ir: %d failure(s)\n", g_fail);
    return 1;
}
