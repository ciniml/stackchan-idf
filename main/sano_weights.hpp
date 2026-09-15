// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS-jp の重み blob (saanotts-jp-*-int8.bin) の永続化とロード。
// 保存先は拡張テーブルの "sanotts" 領域 (raw)。先頭にヘッダ {magic "SANW",
// サイズ, CRC32} を置き、データ部を esp_partition_mmap して jtts::set_sano_weights
// にゼロコピーで渡す (コアは blob を直接参照するので mmap は保持し続ける)。
// 領域が無いボード / CONFIG_JTTS_ENABLE_SANOTTS 無効では全 API が失敗を返し、
// jtts は他エンジンへフォールバックする。
//
// 重みは非 MIT (上流 LICENSE-MODEL.md)。本ファームウェアには同梱せず、利用者が
// 公式 Releases から取得して入れる。
#pragma once

#include <cstdint>
#include <span>

namespace stackchan::app::sano_weights {

// ブート時: sanotts 領域を検証・mmap して jtts に登録する。戻り値はロード成功。
bool init();

// blob を検証して領域に書き込み、その場でロードする (HTTP アップロード /
// 機体側ダウンロード用)。成功で nullptr、失敗で静的なエラーメッセージ。
const char* store(std::span<const std::uint8_t> data);

// 保存済みの重みを消去してアンロードする。
const char* clear();

struct Status {
    bool loaded = false;             // jtts に登録済みか
    std::uint32_t stored_bytes = 0;  // 領域上の blob サイズ (0 = なし)
    std::uint32_t capacity = 0;      // 領域容量 (0 = 領域なし)
};
Status status();

}  // namespace stackchan::app::sano_weights
