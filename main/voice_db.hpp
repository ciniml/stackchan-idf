// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 単位連結 TTS の音声 DB (.jvox) の永続化とロード。
// 保存先は "storage" パーティション上の NVS (avatar_vm と同居、namespace
// "jvox")。保存形式は受け取ったまま (通常 ADPCM ~230 KB、NVS blob 上限
// 508 KB 以内)。ロード時に PSRAM へ展開して jtts::set_voice_db に渡す。
// PSRAM の無いボード (AtomS3) では展開バッファが確保できず、単位連結は
// 使われない (jtts がフォルマント合成へフォールバック)。
#pragma once

#include <cstdint>
#include <span>

namespace stackchan::app::voice_db {

// ブート時: NVS から DB を読み出し、展開して jtts に登録する。
// 戻り値はロードした単位数 (0 = DB なし / ロード不可)。
std::uint16_t init();

// DB を検証して NVS に保存し、その場でロードする (HTTP アップロード用)。
// data は .jvox (ADPCM でも生でも可)。成功で nullptr、失敗で静的な
// エラーメッセージ (HTTP レスポンスにそのまま使える)。
const char* store(std::span<const std::uint8_t> data);

// 保存済み DB を削除してアンロードする。
const char* clear();

struct Status {
    bool loaded = false;          // jtts に登録済みか
    std::uint16_t units = 0;      // ロード中の単位数
    std::uint32_t stored_bytes = 0;  // NVS 上の blob サイズ (0 = なし)
};
Status status();

}  // namespace stackchan::app::voice_db
