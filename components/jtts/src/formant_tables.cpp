// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "internal.hpp"

namespace stackchan::jtts::internal {

FormantFrame vowel_frame(Vowel v, bool palatalized) {
    FormantFrame f;
    f.voicing = 1.0f;
    f.frication = 0.0f;
    f.nasal = 0.0f;
    switch (v) {
        case Vowel::A:
            f.f1 = 800;  f.f2 = 1200; f.f3 = 2600;
            f.bw1 = 80;  f.bw2 = 90;  f.bw3 = 150;
            f.a1 = 1.0f; f.a2 = 0.60f; f.a3 = 0.25f;
            break;
        case Vowel::I:
            f.f1 = 300;  f.f2 = 2300; f.f3 = 3000;
            f.bw1 = 60;  f.bw2 = 100; f.bw3 = 200;
            f.a1 = 1.0f; f.a2 = 0.55f; f.a3 = 0.30f;
            break;
        case Vowel::U:
            f.f1 = 350;  f.f2 = 1300; f.f3 = 2200;
            f.bw1 = 70;  f.bw2 = 100; f.bw3 = 150;
            f.a1 = 0.95f; f.a2 = 0.45f; f.a3 = 0.20f;
            break;
        case Vowel::E:
            f.f1 = 500;  f.f2 = 1900; f.f3 = 2600;
            f.bw1 = 70;  f.bw2 = 100; f.bw3 = 150;
            f.a1 = 1.0f; f.a2 = 0.55f; f.a3 = 0.25f;
            break;
        case Vowel::O:
            f.f1 = 500;  f.f2 = 900;  f.f3 = 2600;
            f.bw1 = 70;  f.bw2 = 90;  f.bw3 = 150;
            f.a1 = 1.0f; f.a2 = 0.50f; f.a3 = 0.20f;
            break;
        case Vowel::None:
            break;
    }
    if (palatalized) {
        f.f2 += 250.0f;
    }
    return f;
}

FormantFrame nasal_frame(Consonant c) {
    FormantFrame f;
    f.voicing = 1.0f;
    f.frication = 0.0f;
    f.nasal = 1.0f;
    f.bw1 = 100;
    f.bw2 = 120;
    f.bw3 = 200;
    f.a1 = 0.55f;
    f.a2 = 0.25f;
    f.a3 = 0.10f;
    // 鼻音ゼロの目標周波数は調音位置で決まる (口腔側枝の長さに反比例):
    // 両唇 /m/ は枝が長くゼロが低い、歯茎 /n/ は高い。V2 レンダラが
    // nasal=1 でゼロをここへ動かし、口腔共鳴の抜けた「鼻にかかった」
    // スペクトルを作る (ん = 口蓋垂鼻音はさらに高く phoneme.cpp で上書き)。
    if (c == Consonant::M) {
        f.f1 = 250; f.f2 = 1100; f.f3 = 2400;
        f.nasal_zero_hz = 1000.0f;
    } else {
        f.f1 = 300; f.f2 = 1700; f.f3 = 2700;
        f.nasal_zero_hz = 1500.0f;
    }
    return f;
}

FormantFrame consonant_burst(Consonant c, Vowel next_v) {
    FormantFrame f;
    f.bw1 = 120;
    f.bw2 = 150;
    f.bw3 = 250;
    f.f0_hz = 130;
    switch (c) {
        case Consonant::K:
        case Consonant::G: {
            float f2_target = vowel_frame(next_v, false).f2;
            f.f1 = 400;
            f.f2 = (f2_target + 1800.0f) * 0.5f;
            f.f3 = 2500;
            if (c == Consonant::K) {
                f.voicing = 0.0f; f.frication = 0.9f;
            } else {
                f.voicing = 0.5f; f.frication = 0.4f;
            }
            f.a1 = 0.3f; f.a2 = 0.6f; f.a3 = 0.4f;
            break;
        }
        case Consonant::T:
        case Consonant::D:
            f.f1 = 350; f.f2 = 1800; f.f3 = 3800;
            if (c == Consonant::T) {
                f.voicing = 0.0f; f.frication = 1.0f;
            } else {
                f.voicing = 0.6f; f.frication = 0.3f;
            }
            f.a1 = 0.2f; f.a2 = 0.4f; f.a3 = 0.7f;
            break;
        case Consonant::P:
        case Consonant::B:
            f.f1 = 350; f.f2 = 900; f.f3 = 2200;
            if (c == Consonant::P) {
                f.voicing = 0.0f; f.frication = 0.7f;
            } else {
                f.voicing = 0.6f; f.frication = 0.2f;
            }
            f.a1 = 0.6f; f.a2 = 0.4f; f.a3 = 0.2f;
            break;
        case Consonant::S:
            f.f1 = 300; f.f2 = 1500; f.f3 = 4500;
            f.bw3 = 600;
            f.voicing = 0.0f; f.frication = 1.0f;
            f.a1 = 0.05f; f.a2 = 0.1f; f.a3 = 1.0f;
            f.fric_shape = FricationShape::Sibilant;
            break;
        case Consonant::Z:
            f.f1 = 300; f.f2 = 1500; f.f3 = 4500;
            f.bw3 = 600;
            f.voicing = 0.7f; f.frication = 0.7f;
            f.a1 = 0.2f; f.a2 = 0.15f; f.a3 = 0.8f;
            f.fric_shape = FricationShape::Sibilant;
            break;
        case Consonant::Sh:
            f.f1 = 350; f.f2 = 1900; f.f3 = 2800;
            f.bw3 = 500;
            f.voicing = 0.0f; f.frication = 1.0f;
            f.a1 = 0.05f; f.a2 = 0.2f; f.a3 = 1.0f;
            f.fric_shape = FricationShape::Palatal;
            break;
        case Consonant::J:
            f.f1 = 350; f.f2 = 1900; f.f3 = 2800;
            f.bw3 = 500;
            f.voicing = 0.7f; f.frication = 0.6f;
            f.a1 = 0.2f; f.a2 = 0.25f; f.a3 = 0.8f;
            f.fric_shape = FricationShape::Palatal;
            break;
        case Consonant::Ts:
            f.f1 = 300; f.f2 = 1500; f.f3 = 4500;
            f.bw3 = 600;
            f.voicing = 0.0f; f.frication = 1.0f;
            f.a1 = 0.05f; f.a2 = 0.1f; f.a3 = 1.0f;
            f.fric_shape = FricationShape::Sibilant;
            break;
        case Consonant::Ch:
            f.f1 = 350; f.f2 = 1900; f.f3 = 2800;
            f.bw3 = 500;
            f.voicing = 0.0f; f.frication = 1.0f;
            f.a1 = 0.05f; f.a2 = 0.2f; f.a3 = 1.0f;
            f.fric_shape = FricationShape::Palatal;
            break;
        case Consonant::H:
        case Consonant::F: {
            FormantFrame v = vowel_frame(next_v, false);
            f.f1 = v.f1; f.f2 = v.f2; f.f3 = v.f3;
            f.voicing = 0.0f; f.frication = 0.4f;
            f.a1 = 0.3f; f.a2 = 0.25f; f.a3 = 0.25f;
            if (c == Consonant::F) {
                f.f3 = 2000;
                f.a3 = 0.4f;
            }
            break;
        }
        case Consonant::Hy:
            f.f1 = 300; f.f2 = 2200; f.f3 = 3000;
            f.voicing = 0.0f; f.frication = 0.5f;
            f.a1 = 0.1f; f.a2 = 0.4f; f.a3 = 0.5f;
            break;
        case Consonant::Y: {
            FormantFrame v = vowel_frame(Vowel::I, false);
            f = v;
            f.a1 *= 0.7f; f.a2 *= 0.7f; f.a3 *= 0.7f;
            break;
        }
        case Consonant::W: {
            FormantFrame v = vowel_frame(Vowel::U, false);
            f = v;
            f.a1 *= 0.7f; f.a2 *= 0.7f; f.a3 *= 0.7f;
            break;
        }
        case Consonant::R: {
            FormantFrame v = vowel_frame(next_v, false);
            f = v;
            f.a1 *= 0.5f; f.a2 *= 0.5f; f.a3 *= 0.5f;
            f.f1 = 400;
            break;
        }
        case Consonant::N:
        case Consonant::M:
            f = nasal_frame(c);
            break;
        case Consonant::None:
            f = vowel_frame(next_v, false);
            break;
    }
    return f;
}

}  // namespace stackchan::jtts::internal
