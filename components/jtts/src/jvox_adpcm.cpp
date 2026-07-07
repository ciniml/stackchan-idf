// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// .jvox codec=1 (IMA-ADPCM 4bit) の展開。エンコードはホスト側
// (tools/jvox/pack_jvox.py --adpcm) で行い、デバイスは展開のみ。
#include <cstring>

#include "jtts/jvox.hpp"

namespace stackchan::jtts::jvox {

namespace {

constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kUnitRecSize = 16;

constexpr int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,   23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,   80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,  279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,  963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024, 3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

std::uint16_t rd_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t rd_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// blob のヘッダを検査して pcm ブロックの手前までのサイズを返す。不正なら 0。
std::size_t prefix_size(std::span<const std::uint8_t> blob) {
    if (blob.size() < kHeaderSize) return 0;
    if (std::memcmp(blob.data(), "JVOX", 4) != 0) return 0;
    if (blob[4] != 1) return 0;  // version
    const std::uint16_t unit_count = rd_u16(blob.data() + 8);
    std::size_t off = kHeaderSize + kUnitRecSize * static_cast<std::size_t>(unit_count);
    if (blob.size() < off + 4) return 0;
    const std::uint32_t marks_total = rd_u32(blob.data() + off);
    off += 4 + 2u * marks_total;
    if (blob.size() < off) return 0;
    return off;
}

}  // namespace

bool is_adpcm(std::span<const std::uint8_t> blob) {
    return prefix_size(blob) != 0 && blob[5] == 1;
}

std::size_t decoded_size(std::span<const std::uint8_t> blob) {
    const std::size_t pre = prefix_size(blob);
    if (pre == 0 || blob[5] != 1) return 0;
    if (blob.size() < pre + 4) return 0;
    const std::uint32_t n = rd_u32(blob.data() + pre);
    if (blob.size() < pre + 4 + (n + 1) / 2) return 0;
    return pre + 2u * static_cast<std::size_t>(n);
}

bool decode_adpcm(std::span<const std::uint8_t> blob, std::span<std::uint8_t> out) {
    const std::size_t pre = prefix_size(blob);
    const std::size_t want = decoded_size(blob);
    if (want == 0 || out.size() != want) return false;

    // ヘッダ + UnitRec + marks をそのままコピーして codec を 0 に。
    std::memcpy(out.data(), blob.data(), pre);
    out[5] = 0;

    const std::uint32_t n = rd_u32(blob.data() + pre);
    const std::uint8_t* nib = blob.data() + pre + 4;
    std::uint8_t* dst = out.data() + pre;

    std::int32_t pred = 0;
    int index = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint8_t byte = nib[i / 2];
        const std::uint8_t code = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
        const int step = kStepTable[index];
        std::int32_t diff = step >> 3;
        if (code & 1) diff += step >> 2;
        if (code & 2) diff += step >> 1;
        if (code & 4) diff += step;
        if (code & 8) diff = -diff;
        pred += diff;
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        index += kIndexTable[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        dst[2 * i] = static_cast<std::uint8_t>(pred & 0xFF);
        dst[2 * i + 1] = static_cast<std::uint8_t>((pred >> 8) & 0xFF);
    }
    return true;
}

}  // namespace stackchan::jtts::jvox
