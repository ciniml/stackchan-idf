// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "scs_servo/scs_bus.hpp"

#include <array>
#include <cstddef>
#include <cstring>

#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::scs_servo {

namespace {

constexpr const char* kTag = "scs_bus";
constexpr std::size_t kRxBufferBytes = 256;
constexpr std::size_t kMaxParams = 32;
constexpr std::size_t kHeaderBytes = 4; // 0xFF 0xFF | ID | Length
constexpr std::uint8_t kStatusErrorMask = 0x7F;

std::uint8_t make_checksum(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t sum = 0;
    for (auto b : bytes) {
        sum += b;
    }
    return static_cast<std::uint8_t>(~sum);
}

} // namespace

tl::expected<ScsBus, ScsError> ScsBus::create(const Config& config)
{
    const uart_config_t uart_config = {
        .baud_rate = static_cast<int>(config.baud),
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };

    if (uart_driver_install(config.uart, kRxBufferBytes, 0, 0, nullptr, 0) != ESP_OK) {
        return tl::unexpected{ScsError::UartInit};
    }
    if (uart_param_config(config.uart, &uart_config) != ESP_OK) {
        uart_driver_delete(config.uart);
        return tl::unexpected{ScsError::UartInit};
    }
    if (uart_set_pin(config.uart, config.tx, config.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(config.uart);
        return tl::unexpected{ScsError::UartInit};
    }

    ScsBus bus;
    bus.uart_ = config.uart;
    bus.timeout_ms_ = config.timeout_ms;
    bus.echo_cancel_ = config.echo_cancel;
    return bus;
}

ScsBus::ScsBus(ScsBus&& other) noexcept
    : uart_{other.uart_}, timeout_ms_{other.timeout_ms_}, echo_cancel_{other.echo_cancel_}
{
    other.uart_ = kInvalidUart;
}

ScsBus& ScsBus::operator=(ScsBus&& other) noexcept
{
    if (this != &other) {
        release();
        uart_ = other.uart_;
        timeout_ms_ = other.timeout_ms_;
        echo_cancel_ = other.echo_cancel_;
        other.uart_ = kInvalidUart;
    }
    return *this;
}

ScsBus::~ScsBus()
{
    release();
}

void ScsBus::release() noexcept
{
    if (uart_ != kInvalidUart) {
        uart_driver_delete(uart_);
        uart_ = kInvalidUart;
    }
}

tl::expected<void, ScsError>
ScsBus::send(std::uint8_t id, std::uint8_t instruction, std::span<const std::uint8_t> params)
{
    if (params.size() > kMaxParams) {
        return tl::unexpected{ScsError::BufferTooSmall};
    }

    std::array<std::uint8_t, kMaxParams + 6> tx{};
    tx[0] = 0xFF;
    tx[1] = 0xFF;
    tx[2] = id;
    tx[3] = static_cast<std::uint8_t>(params.size() + 2); // instruction + params + checksum
    tx[4] = instruction;
    std::memcpy(tx.data() + 5, params.data(), params.size());
    tx[5 + params.size()] = make_checksum(std::span{tx.data() + 2, 3 + params.size()});

    const std::size_t total = 6 + params.size();
    uart_flush_input(uart_);
    const int written = uart_write_bytes(uart_, tx.data(), total);
    if (written < 0 || static_cast<std::size_t>(written) != total) {
        return tl::unexpected{ScsError::UartInit};
    }
    if (uart_wait_tx_done(uart_, pdMS_TO_TICKS(timeout_ms_)) != ESP_OK) {
        return tl::unexpected{ScsError::Timeout};
    }
    if (echo_cancel_) {
        // Half-duplex: our own transmission echoes back onto RX. Read and drop
        // exactly the bytes we just sent so the caller (transact / next send)
        // sees only the servo's response.
        std::array<std::uint8_t, kMaxParams + 6> echo{};
        const int got = uart_read_bytes(uart_, echo.data(), total, pdMS_TO_TICKS(timeout_ms_));
        if (got != static_cast<int>(total)) {
            return tl::unexpected{ScsError::Timeout};
        }
    }
    return {};
}

tl::expected<std::span<const std::uint8_t>, ScsError>
ScsBus::transact(std::uint8_t id, std::uint8_t instruction, std::span<const std::uint8_t> params,
                 std::span<std::uint8_t> rx_scratch)
{
    if (auto r = send(id, instruction, params); !r) {
        return tl::unexpected{r.error()};
    }

    // Receive header: 0xFF 0xFF | ID | Length
    std::array<std::uint8_t, kHeaderBytes> header{};
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms_);

    int n = uart_read_bytes(uart_, header.data(), header.size(), ticks);
    if (n != static_cast<int>(header.size())) {
        return tl::unexpected{ScsError::Timeout};
    }
    if (header[0] != 0xFF || header[1] != 0xFF) {
        return tl::unexpected{ScsError::BadHeader};
    }
    if (header[2] != id) {
        return tl::unexpected{ScsError::IdMismatch};
    }

    const std::uint8_t length = header[3];
    if (length < 2) {
        return tl::unexpected{ScsError::BadLength};
    }
    const std::size_t body_len = length - 1; // error + params + checksum, minus checksum below
    if (body_len < 1) {
        return tl::unexpected{ScsError::BadLength};
    }
    const std::size_t data_len = body_len - 1; // subtract the error byte
    if (data_len > rx_scratch.size()) {
        return tl::unexpected{ScsError::BufferTooSmall};
    }

    std::array<std::uint8_t, 1> err_byte{};
    n = uart_read_bytes(uart_, err_byte.data(), err_byte.size(), ticks);
    if (n != 1) {
        return tl::unexpected{ScsError::Timeout};
    }

    if (data_len > 0) {
        n = uart_read_bytes(uart_, rx_scratch.data(), data_len, ticks);
        if (n != static_cast<int>(data_len)) {
            return tl::unexpected{ScsError::Timeout};
        }
    }

    std::array<std::uint8_t, 1> cks_byte{};
    n = uart_read_bytes(uart_, cks_byte.data(), 1, ticks);
    if (n != 1) {
        return tl::unexpected{ScsError::Timeout};
    }

    // Verify checksum over ID, Length, Error, Params.
    std::uint32_t sum = static_cast<std::uint32_t>(header[2]) + header[3] + err_byte[0];
    for (std::size_t i = 0; i < data_len; ++i) {
        sum += rx_scratch[i];
    }
    const std::uint8_t expected = static_cast<std::uint8_t>(~sum);
    if (expected != cks_byte[0]) {
        return tl::unexpected{ScsError::ChecksumMismatch};
    }

    if ((err_byte[0] & kStatusErrorMask) != 0) {
        ESP_LOGW(kTag, "servo id=%u reports error=0x%02X", id, err_byte[0]);
        return tl::unexpected{ScsError::ServoError};
    }

    return std::span<const std::uint8_t>{rx_scratch.data(), data_len};
}

} // namespace stackchan::scs_servo
