# sanoTTS-jp 推論コア (ベンダリング)

- 上流: https://github.com/ayutaz/sanoTTS-jp
- 取り込み: v1.1.0 (commit 6a59fc6fe1b435ccd5fa33449167f123a19fb0ca、2026-09-13)
- ライセンス: MIT (同梱の `LICENSE`)。**モデルの重みは同梱していない** (重みは非 MIT、
  `LICENSE-MODEL.md` の条件で公式 Releases から利用者が取得する)。

取り込んだファイル (上流 `csrc/` からそのまま。**改変しない**。更新は上流の
同名ファイルを上書きしてこの版数を書き換える):

| ファイル | 役割 |
|---|---|
| `saanotts.c/.h`, `saanotts_internal.h` | 一括合成コア、重み blob の読み出し、arena |
| `saanotts_stream.c/.h` | ストリーミング合成 (176 KB arena) |
| `saanotts_int8.c/.h` | int8 カーネル (W8A32 スカラー / W8A8 + PIE) |
| `fft.c/.h`, `erf_table.h`, `saan_prof.h` | iSTFT、GELU の erf 表、プロファイラ |
| `g2p.c/.h`, `g2p_table.h` | かな中間表現 → 音素 ID 列 (辞書不要の経路) |
| `golden_test.c`, `stream_test.c` | 上流の受け入れテスト (ホストテストで使う) |

取り込んでいないもの: 漢字経路 (`jdict.c`, `accent.c`, `njd_rules.c`, `label_ids.c`,
`openjtalk/`。辞書 13.7 MB が要る)、行編集 (`line.c`)、公式 ESP32 ファームウェア。
