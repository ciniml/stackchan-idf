// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <tl/expected.hpp>

#include <mbedtls/gcm.h>

#include "config_service/config_service.hpp"

namespace stackchan::config::crypto {

// Application-layer encrypted session for the BLE settings service.
//
// Protocol:
//   1. The central reads the device's X25519 public key from the KeyExchange
//      characteristic. The first read generates a fresh ephemeral keypair.
//   2. The central generates its own X25519 keypair and writes its public
//      key (32 bytes) to KeyExchange.
//   3. Both sides ECDH → 32-byte shared secret → HKDF-SHA256
//      (info="stackchan-config-v1") → 32-byte AES-256 key.
//   4. All subsequent reads/writes of the settings characteristics carry
//      [12-byte nonce][ciphertext][16-byte GCM tag].
//
// Session keys are ephemeral: a fresh keypair on each BLE connection, all
// material zeroized on disconnect. No persistent bonding, no MITM defence.
class Session {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // Lazily generate the device's X25519 keypair on first call. Returns a
    // view onto the cached 32-byte public key (RFC 7748 little-endian X).
    tl::expected<std::span<const std::uint8_t>, Error> ensure_device_keypair();

    // Combine our private scalar with the peer's 32-byte public key, derive
    // the AES-256 session key, mark the session established. Requires
    // ensure_device_keypair() to have run.
    tl::expected<void, Error> complete_handshake(std::span<const std::uint8_t, 32> peer_pub);

    bool is_established() const noexcept { return established_; }

    // Encrypt plaintext into [12B nonce][ciphertext][16B tag]. The nonce is
    // freshly random per call.
    tl::expected<std::vector<std::uint8_t>, Error> encrypt(std::span<const std::uint8_t> plaintext);

    // Decrypt [12B nonce][ciphertext][16B tag] into plaintext bytes.
    tl::expected<std::vector<std::uint8_t>, Error> decrypt(std::span<const std::uint8_t> wire);

    // Drop all key material. Call on disconnect.
    void reset();

private:
    bool keypair_ready_{false};
    bool established_{false};
    std::array<std::uint8_t, 32> device_pub_{};
    std::array<std::uint8_t, 32> device_priv_{};
    std::array<std::uint8_t, 32> aes_key_{};
    mbedtls_gcm_context gcm_;
};

} // namespace stackchan::config::crypto
