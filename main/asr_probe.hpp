// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once
// [ASR Phase 0/1] esp-sr AFE + WakeNet のブリングアップ。ウェイクワード
// 検出時に SharedState 経由で表情+吹き出しを駆動する。
// CONFIG_STACKCHAN_ASR_ENABLED の時だけ app_main から一度呼ぶ。
namespace stackchan::app {
class SharedState;
}
void asr_probe_run(stackchan::app::SharedState& state);
