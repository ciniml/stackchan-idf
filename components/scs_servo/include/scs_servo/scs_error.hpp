// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

namespace stackchan::scs_servo {

enum class ScsError {
    UartInit,
    Timeout,
    BadHeader,
    BadLength,
    ChecksumMismatch,
    IdMismatch,
    ServoError,
    BufferTooSmall,
    InvalidArgument,
};

} // namespace stackchan::scs_servo
