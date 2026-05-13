// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <tl/expected.hpp>
#include <memory>

#include <M5GFX.h>

namespace stackchan::board {

enum class Error {
    PmicInit,
    DisplayInit,
    ExpanderProbe,
    ExpanderWrite,
};

class Board {
public:
    static tl::expected<Board, Error> begin();

    Board(Board&&) noexcept = default;
    Board& operator=(Board&&) noexcept = default;
    Board(const Board&) = default;
    Board& operator=(const Board&) = default;
    ~Board() = default;

    M5GFX& display() noexcept;

    tl::expected<void, Error> set_servo_power(bool on);

private:
    Board() = default;
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace stackchan::board
