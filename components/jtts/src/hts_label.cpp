// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// かな文字列 → HTS full-context ラベル生成 (辞書・形態素解析なし)。
// Python リファレンス実装: tools/jvox/hts_label_kana.py (同一ロジック)。
//
// 品詞系フィールド (B/C/D) は xx、アクセントは無指定なら平板型 (type 0)。
// 読み仕様:
//   、。，,．.  … 呼気段落境界 (間にポーズが入る)
//   /          … アクセント句境界 (ポーズなし)
//   '          … 直前モーラをアクセント核にする (例: きょ'うの/て'んきは)
//   末尾の？?  … 疑問文フラグ (F3)
#include <cstdio>
#include <string>
#include <vector>

#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

struct AccentPhrase {
    std::vector<Mora> moras;
    int accent = 0;  // 0 = 平板
    bool interrogative = false;
};

struct BreathGroup {
    std::vector<AccentPhrase> phrases;
    std::size_t mora_count() const {
        std::size_t n = 0;
        for (const auto& p : phrases) n += p.moras.size();
        return n;
    }
};

// Mora → HTS 音素名列。無声化母音は大文字。
void mora_phonemes(const Mora& m, Vowel prev_vowel, std::vector<std::string>& out) {
    switch (m.kind) {
        case MoraKind::MoraicN:
            out.push_back("N");
            return;
        case MoraKind::Sokuon:
            out.push_back("cl");
            return;
        case MoraKind::Chouon: {
            Vowel v = (prev_vowel == Vowel::None) ? Vowel::U : prev_vowel;
            static constexpr const char* kLower[] = {"", "a", "i", "u", "e", "o"};
            static constexpr const char* kUpper[] = {"", "A", "I", "U", "E", "O"};
            out.push_back((m.devoiced ? kUpper : kLower)[static_cast<int>(v)]);
            return;
        }
        case MoraKind::CV:
            break;
    }
    const char* c = nullptr;
    switch (m.c) {
        case Consonant::None: break;
        case Consonant::K:  c = m.palatalized ? "ky" : "k"; break;
        case Consonant::G:  c = m.palatalized ? "gy" : "g"; break;
        case Consonant::S:  c = "s"; break;
        case Consonant::Z:  c = "z"; break;
        case Consonant::Sh: c = "sh"; break;
        case Consonant::J:  c = "j"; break;
        case Consonant::T:  c = "t"; break;
        case Consonant::D:  c = "d"; break;
        case Consonant::Ts: c = "ts"; break;
        case Consonant::Ch: c = "ch"; break;
        case Consonant::N:  c = m.palatalized ? "ny" : "n"; break;
        case Consonant::H:  c = "h"; break;
        case Consonant::F:  c = "f"; break;
        // ひ = Hy+I は Open JTalk では "h i"、ひゃ行 (palatalized) は "hy"
        case Consonant::Hy: c = m.palatalized ? "hy" : "h"; break;
        case Consonant::B:  c = m.palatalized ? "by" : "b"; break;
        case Consonant::P:  c = m.palatalized ? "py" : "p"; break;
        case Consonant::M:  c = m.palatalized ? "my" : "m"; break;
        case Consonant::Y:  c = "y"; break;
        case Consonant::R:  c = m.palatalized ? "ry" : "r"; break;
        case Consonant::W:  c = "w"; break;
    }
    if (c != nullptr) out.push_back(c);
    static constexpr const char* kLower[] = {"", "a", "i", "u", "e", "o"};
    static constexpr const char* kUpper[] = {"", "A", "I", "U", "E", "O"};
    if (m.v != Vowel::None) out.push_back((m.devoiced ? kUpper : kLower)[static_cast<int>(m.v)]);
}

bool is_bg_break(char32_t ch) {
    return ch == U'、' || ch == U'。' || ch == U'，' || ch == U',' || ch == U'．' || ch == U'.';
}

// かな文字列 → 呼気段落 / アクセント句構造
bool parse_hts_text(std::u32string_view text, std::vector<BreathGroup>& groups) {
    groups.clear();
    BreathGroup bg;
    std::u32string cur;
    int accent = 0;
    bool interrogative = false;

    auto flush_ap = [&]() -> bool {
        if (cur.empty()) return true;
        AccentPhrase ap;
        if (!parse_kana(cur, ap.moras)) {
            cur.clear();
            accent = 0;
            return true;  // 記号だけの句などは無視
        }
        apply_devoicing(ap.moras);
        ap.accent = (accent > 0 && accent <= static_cast<int>(ap.moras.size())) ? accent : 0;
        bg.phrases.push_back(std::move(ap));
        cur.clear();
        accent = 0;
        return true;
    };
    auto flush_bg = [&]() {
        flush_ap();
        if (!bg.phrases.empty()) groups.push_back(std::move(bg));
        bg = BreathGroup{};
    };

    for (char32_t ch : text) {
        if (ch == U'？' || ch == U'?') {
            interrogative = true;
            continue;
        }
        if (ch == U'！' || ch == U'!') continue;
        if (is_bg_break(ch)) {
            flush_bg();
            continue;
        }
        if (ch == U'/') {
            flush_ap();
            continue;
        }
        if (ch == U'\'' || ch == U'’') {
            // ここまでのモーラ数 = 核位置
            std::vector<Mora> tmp;
            if (parse_kana(cur, tmp)) accent = static_cast<int>(tmp.size());
            continue;
        }
        cur.push_back(ch);
    }
    flush_bg();

    if (groups.empty()) return false;
    if (interrogative) groups.back().phrases.back().interrogative = true;
    return true;
}

struct PhEntry {
    std::string ph;
    bool sil = false;
    int gi = -1;  // breath group index (pau は直前 BG の index)
    int pi = -1;
    int a1 = 0, a2 = 0, a3 = 0;
    int f5 = 0, f6 = 0, f7 = 0, f8 = 0;
    int ap_global = -1;
};

}  // namespace

bool build_hts_labels(std::u32string_view text, std::vector<std::string>& labels) {
    labels.clear();
    std::vector<BreathGroup> groups;
    if (!parse_hts_text(text, groups)) return false;

    const int total_bg = static_cast<int>(groups.size());
    int total_ap = 0;
    int total_mora = 0;
    std::vector<std::pair<int, int>> ap_flat;  // (gi, pi)
    for (int gi = 0; gi < total_bg; ++gi) {
        for (int pi = 0; pi < static_cast<int>(groups[gi].phrases.size()); ++pi) {
            ap_flat.emplace_back(gi, pi);
            ++total_ap;
            total_mora += static_cast<int>(groups[gi].phrases[pi].moras.size());
        }
    }

    // 音素平坦列の構築
    std::vector<PhEntry> entries;
    {
        PhEntry sil;
        sil.ph = "sil";
        sil.sil = true;
        entries.push_back(sil);
    }
    int mora_global = 0;
    for (int gi = 0; gi < total_bg; ++gi) {
        const auto& bg = groups[gi];
        const int bg_mora = static_cast<int>(bg.mora_count());
        int mora_in_bg = 0;
        int ap_global_base = 0;
        for (int g2 = 0; g2 < gi; ++g2) ap_global_base += static_cast<int>(groups[g2].phrases.size());
        for (int pi = 0; pi < static_cast<int>(bg.phrases.size()); ++pi) {
            const auto& ap = bg.phrases[pi];
            const int n_mora = static_cast<int>(ap.moras.size());
            const int acc = ap.accent;
            Vowel prev_vowel = Vowel::None;
            for (int mi = 0; mi < n_mora; ++mi) {
                const int pos = mi + 1;
                // 平板 (type 0) は全モーラ「核より前」扱い (hts_label_kana.py と同一)
                const int a1 = (acc > 0) ? pos - acc : pos - n_mora - 1;
                std::vector<std::string> phs;
                mora_phonemes(ap.moras[mi], prev_vowel, phs);
                if (ap.moras[mi].kind == MoraKind::CV && ap.moras[mi].v != Vowel::None) {
                    prev_vowel = ap.moras[mi].v;
                }
                for (auto& p : phs) {
                    PhEntry e;
                    e.ph = std::move(p);
                    e.gi = gi;
                    e.pi = pi;
                    e.a1 = a1;
                    e.a2 = pos;
                    e.a3 = n_mora - pos + 1;
                    e.f5 = pi + 1;
                    e.f6 = static_cast<int>(bg.phrases.size()) - pi;
                    e.f7 = mora_in_bg + mi + 1;
                    e.f8 = bg_mora - (mora_in_bg + mi);
                    e.ap_global = ap_global_base + pi;
                    entries.push_back(std::move(e));
                }
            }
            mora_in_bg += n_mora;
        }
        mora_global += bg_mora;
        if (gi != total_bg - 1) {
            PhEntry pau;
            pau.ph = "pau";
            pau.sil = true;
            pau.gi = gi;
            entries.push_back(pau);
        }
    }
    {
        PhEntry sil;
        sil.ph = "sil";
        sil.sil = true;
        entries.push_back(sil);
    }

    auto get_ap = [&](int idx) -> const AccentPhrase* {
        if (idx < 0 || idx >= static_cast<int>(ap_flat.size())) return nullptr;
        return &groups[ap_flat[idx].first].phrases[ap_flat[idx].second];
    };
    // (n_mora, accent, interrogative) を "5", "0", "1" 形式の文字列で
    auto ap_ctx = [&](const AccentPhrase* ap, char out[3][16]) {
        if (ap == nullptr) {
            std::snprintf(out[0], 16, "xx");
            std::snprintf(out[1], 16, "xx");
            std::snprintf(out[2], 16, "xx");
        } else {
            std::snprintf(out[0], 16, "%d", static_cast<int>(ap->moras.size()));
            std::snprintf(out[1], 16, "%d", ap->accent);
            std::snprintf(out[2], 16, "%d", ap->interrogative ? 1 : 0);
        }
    };

    char buf[512];
    char kbuf[48];
    std::snprintf(kbuf, sizeof(kbuf), "/K:%d+%d-%d", total_bg, total_ap, total_mora);

    const int n = static_cast<int>(entries.size());
    for (int i = 0; i < n; ++i) {
        const auto& e = entries[i];
        const char* p1 = (i >= 2) ? entries[i - 2].ph.c_str() : "xx";
        const char* p2 = (i >= 1) ? entries[i - 1].ph.c_str() : "xx";
        const char* p3 = e.ph.c_str();
        const char* p4 = (i + 1 < n) ? entries[i + 1].ph.c_str() : "xx";
        const char* p5 = (i + 2 < n) ? entries[i + 2].ph.c_str() : "xx";

        if (e.sil) {
            char ec[3][16], gc[3][16];
            if (e.ph == "sil" && i == 0) {
                ap_ctx(get_ap(0), gc);
                std::snprintf(buf, sizeof(buf),
                              "%s^%s-%s+%s=%s/A:xx+xx+xx/B:xx-xx_xx/C:xx_xx+xx/D:xx+xx_xx"
                              "/E:xx_xx!xx_xx-xx/F:xx_xx#xx_xx@xx_xx|xx_xx"
                              "/G:%s_%s%%%s_xx_xx/H:xx_xx/I:xx-xx@xx+xx&xx-xx|xx+xx/J:%d_%d%s",
                              p1, p2, p3, p4, p5, gc[0], gc[1], gc[2],
                              static_cast<int>(groups.front().phrases.size()),
                              static_cast<int>(groups.front().mora_count()), kbuf);
            } else if (e.ph == "sil") {
                ap_ctx(get_ap(total_ap - 1), ec);
                std::snprintf(buf, sizeof(buf),
                              "%s^%s-%s+%s=%s/A:xx+xx+xx/B:xx-xx_xx/C:xx_xx+xx/D:xx+xx_xx"
                              "/E:%s_%s!%s_xx-xx/F:xx_xx#xx_xx@xx_xx|xx_xx"
                              "/G:xx_xx%%xx_xx_xx/H:%d_%d/I:xx-xx@xx+xx&xx-xx|xx+xx/J:xx_xx%s",
                              p1, p2, p3, p4, p5, ec[0], ec[1], ec[2],
                              static_cast<int>(groups.back().phrases.size()),
                              static_cast<int>(groups.back().mora_count()), kbuf);
            } else {  // pau
                const int gi = e.gi;
                ap_ctx(&groups[gi].phrases.back(), ec);
                ap_ctx(&groups[gi + 1].phrases.front(), gc);
                std::snprintf(buf, sizeof(buf),
                              "%s^%s-%s+%s=%s/A:xx+xx+xx/B:xx-xx_xx/C:xx_xx+xx/D:xx+xx_xx"
                              "/E:%s_%s!%s_xx-xx/F:xx_xx#xx_xx@xx_xx|xx_xx"
                              "/G:%s_%s%%%s_xx_xx/H:%d_%d/I:xx-xx@xx+xx&xx-xx|xx+xx/J:%d_%d%s",
                              p1, p2, p3, p4, p5, ec[0], ec[1], ec[2], gc[0], gc[1], gc[2],
                              static_cast<int>(groups[gi].phrases.size()),
                              static_cast<int>(groups[gi].mora_count()),
                              static_cast<int>(groups[gi + 1].phrases.size()),
                              static_cast<int>(groups[gi + 1].mora_count()), kbuf);
            }
            labels.emplace_back(buf);
            continue;
        }

        const int gi = e.gi;
        const int pi = e.pi;
        const auto& bg = groups[gi];
        const auto& ap = bg.phrases[pi];
        const AccentPhrase* prev_ap = get_ap(e.ap_global - 1);
        const AccentPhrase* next_ap = get_ap(e.ap_global + 1);
        char ec[3][16], gc[3][16];
        ap_ctx(prev_ap, ec);
        ap_ctx(next_ap, gc);
        // E5/G5: 隣接 AP との間にポーズがあるか (別 BG なら 1)
        char e5[4] = "xx", g5[4] = "xx";
        if (prev_ap != nullptr)
            std::snprintf(e5, sizeof(e5), "%d", ap_flat[e.ap_global - 1].first != gi ? 1 : 0);
        if (next_ap != nullptr)
            std::snprintf(g5, sizeof(g5), "%d", ap_flat[e.ap_global + 1].first != gi ? 1 : 0);

        char hbuf[24], jbuf[24];
        if (gi > 0)
            std::snprintf(hbuf, sizeof(hbuf), "%d_%d", static_cast<int>(groups[gi - 1].phrases.size()),
                          static_cast<int>(groups[gi - 1].mora_count()));
        else
            std::snprintf(hbuf, sizeof(hbuf), "xx_xx");
        if (gi + 1 < total_bg)
            std::snprintf(jbuf, sizeof(jbuf), "%d_%d", static_cast<int>(groups[gi + 1].phrases.size()),
                          static_cast<int>(groups[gi + 1].mora_count()));
        else
            std::snprintf(jbuf, sizeof(jbuf), "xx_xx");

        int aps_before = 0, morae_before = 0;
        for (int g2 = 0; g2 < gi; ++g2) {
            aps_before += static_cast<int>(groups[g2].phrases.size());
            morae_before += static_cast<int>(groups[g2].mora_count());
        }

        std::snprintf(buf, sizeof(buf),
                      "%s^%s-%s+%s=%s/A:%d+%d+%d/B:xx-xx_xx/C:xx_xx+xx/D:xx+xx_xx"
                      "/E:%s_%s!%s_xx-%s/F:%d_%d#%d_xx@%d_%d|%d_%d/G:%s_%s%%%s_xx_%s"
                      "/H:%s/I:%d-%d@%d+%d&%d-%d|%d+%d/J:%s%s",
                      p1, p2, p3, p4, p5, e.a1, e.a2, e.a3,
                      ec[0], ec[1], ec[2], e5,
                      static_cast<int>(ap.moras.size()), ap.accent, ap.interrogative ? 1 : 0,
                      e.f5, e.f6, e.f7, e.f8,
                      gc[0], gc[1], gc[2], g5,
                      hbuf,
                      static_cast<int>(bg.phrases.size()), static_cast<int>(bg.mora_count()),
                      gi + 1, total_bg - gi,
                      aps_before + pi + 1, total_ap - (aps_before + pi),
                      morae_before + e.f7, total_mora - (morae_before + e.f7 - 1),
                      jbuf, kbuf);
        labels.emplace_back(buf);
    }
    return true;
}

}  // namespace stackchan::jtts::internal
