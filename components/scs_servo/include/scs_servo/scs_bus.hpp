// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <tl/expected.hpp>
#include <span>

#include <driver/gpio.h>
#include <driver/uart.h>

#include "scs_servo/scs_error.hpp"

namespace stackchan::scs_servo {

class ScsBus {
public:
    struct Config {
        uart_port_t uart;
        gpio_num_t tx;
        gpio_num_t rx;
        std::uint32_t baud = 1'000'000;
        std::uint32_t timeout_ms = 20;
        // Half-duplex buses (e.g. the Takao base) feed the controller's own TX
        // back onto RX. When true, each send() drains that echo (the bytes it
        // just transmitted) before any response is read.
        bool echo_cancel = false;
    };

    static tl::expected<ScsBus, ScsError> create(const Config& config);

    ScsBus(const ScsBus&) = delete;
    ScsBus& operator=(const ScsBus&) = delete;
    ScsBus(ScsBus&& other) noexcept;
    ScsBus& operator=(ScsBus&& other) noexcept;
    ~ScsBus();

    // Send a request and receive the status packet. `params` is the payload that
    // follows the Instruction byte. `rx_scratch` is the caller-supplied buffer
    // for the response data field; the returned span aliases it.
    tl::expected<std::span<const std::uint8_t>, ScsError>
    transact(std::uint8_t id, std::uint8_t instruction, std::span<const std::uint8_t> params,
             std::span<std::uint8_t> rx_scratch);

    // Send-only (for broadcast ID 0xFE or sync writes).
    tl::expected<void, ScsError>
    send(std::uint8_t id, std::uint8_t instruction, std::span<const std::uint8_t> params);

    uart_port_t uart() const noexcept { return uart_; }

private:
    static constexpr uart_port_t kInvalidUart = static_cast<uart_port_t>(-1);

    ScsBus() = default;

    void release() noexcept;

    uart_port_t uart_{kInvalidUart};
    std::uint32_t timeout_ms_{20};
    bool echo_cancel_{false};
};

} // namespace stackchan::scs_servo
