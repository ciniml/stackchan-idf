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
