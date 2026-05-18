// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/crypto.hpp>

#include <cstring>
#include <mutex>

#include <esp_log.h>
#include <esp_random.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

namespace stackchan::config::crypto {

namespace {

constexpr const char* kTag = "cfg-crypto";
constexpr const char* kHkdfInfo = "stackchan-config-v1";
constexpr std::size_t kNonceLen = 12;
constexpr std::size_t kTagLen = 16;

// Lazily-seeded DRBG, shared across all Sessions for the process lifetime.
// std::call_once seeds it on first use; touched only from the NimBLE host
// task in practice.
mbedtls_entropy_context g_entropy;
mbedtls_ctr_drbg_context g_drbg;
std::once_flag g_drbg_once;
bool g_drbg_ready = false;

void init_drbg_once()
{
    std::call_once(g_drbg_once, [] {
        mbedtls_entropy_init(&g_entropy);
        mbedtls_ctr_drbg_init(&g_drbg);
        const char* pers = "stackchan-cfg-v1";
        int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                        reinterpret_cast<const unsigned char*>(pers),
                                        std::strlen(pers));
        if (rc != 0) {
            ESP_LOGE(kTag, "ctr_drbg_seed failed: -0x%04x", -rc);
            return;
        }
        g_drbg_ready = true;
    });
}

} // namespace

Session::Session()
{
    mbedtls_gcm_init(&gcm_);
}

Session::~Session()
{
    reset();
}

tl::expected<std::span<const std::uint8_t>, Error> Session::ensure_device_keypair()
{
    if (keypair_ready_) {
        return std::span<const std::uint8_t>{device_pub_};
    }
    init_drbg_once();
    if (!g_drbg_ready) {
        return tl::unexpected{Error::CryptoRng};
    }

    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    Error err = Error::CryptoBadKey;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;
        if (mbedtls_ecdh_gen_public(&grp, &d, &Q, mbedtls_ctr_drbg_random, &g_drbg) != 0) break;
        // Curve25519 represents the public key as the X coordinate, RFC 7748
        // little-endian byte order. mbedtls stores mpi limbs little-endian
        // internally; write_binary_le matches Web Crypto's raw export format.
        if (mbedtls_mpi_write_binary_le(&Q.private_X, device_pub_.data(), 32) != 0) break;
        if (mbedtls_mpi_write_binary_le(&d, device_priv_.data(), 32) != 0) break;
        keypair_ready_ = true;
        err = Error::CryptoNotReady; // sentinel, not used below
    } while (false);

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);

    if (!keypair_ready_) {
        return tl::unexpected{err};
    }
    return std::span<const std::uint8_t>{device_pub_};
}

tl::expected<void, Error> Session::complete_handshake(std::span<const std::uint8_t, 32> peer_pub)
{
    if (!keypair_ready_) {
        return tl::unexpected{Error::CryptoNotReady};
    }
    init_drbg_once();
    if (!g_drbg_ready) {
        return tl::unexpected{Error::CryptoRng};
    }

    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point Qp;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Qp);

    std::array<std::uint8_t, 32> shared{};
    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;
        if (mbedtls_mpi_read_binary_le(&d, device_priv_.data(), 32) != 0) break;
        if (mbedtls_mpi_read_binary_le(&Qp.private_X, peer_pub.data(), 32) != 0) break;
        if (mbedtls_mpi_lset(&Qp.private_Z, 1) != 0) break;
        if (mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d,
                                         mbedtls_ctr_drbg_random, &g_drbg) != 0) break;
        if (mbedtls_mpi_write_binary_le(&z, shared.data(), 32) != 0) break;
        ok = true;
    } while (false);

    mbedtls_ecp_point_free(&Qp);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);

    if (!ok) {
        mbedtls_platform_zeroize(shared.data(), shared.size());
        return tl::unexpected{Error::CryptoBadKey};
    }

    // HKDF-SHA256, no salt, info = "stackchan-config-v1".
    const auto* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == nullptr) {
        mbedtls_platform_zeroize(shared.data(), shared.size());
        return tl::unexpected{Error::CryptoBadKey};
    }
    int rc = mbedtls_hkdf(md_info,
                          nullptr, 0,
                          shared.data(), shared.size(),
                          reinterpret_cast<const unsigned char*>(kHkdfInfo), std::strlen(kHkdfInfo),
                          aes_key_.data(), aes_key_.size());
    mbedtls_platform_zeroize(shared.data(), shared.size());
    if (rc != 0) {
        ESP_LOGW(kTag, "hkdf failed: -0x%04x", -rc);
        return tl::unexpected{Error::CryptoBadKey};
    }

    // The private scalar is no longer needed after deriving the AES key.
    mbedtls_platform_zeroize(device_priv_.data(), device_priv_.size());

    rc = mbedtls_gcm_setkey(&gcm_, MBEDTLS_CIPHER_ID_AES, aes_key_.data(), 256);
    if (rc != 0) {
        ESP_LOGW(kTag, "gcm_setkey failed: -0x%04x", -rc);
        return tl::unexpected{Error::CryptoBadKey};
    }

    established_ = true;
    ESP_LOGI(kTag, "session established (X25519 + AES-256-GCM)");
    return {};
}

tl::expected<std::vector<std::uint8_t>, Error> Session::encrypt(std::span<const std::uint8_t> plaintext)
{
    if (!established_) {
        return tl::unexpected{Error::CryptoNotReady};
    }
    std::vector<std::uint8_t> wire(kNonceLen + plaintext.size() + kTagLen);
    std::uint8_t* nonce = wire.data();
    std::uint8_t* ct = wire.data() + kNonceLen;
    std::uint8_t* tag = wire.data() + kNonceLen + plaintext.size();
    esp_fill_random(nonce, kNonceLen);

    int rc = mbedtls_gcm_crypt_and_tag(&gcm_, MBEDTLS_GCM_ENCRYPT,
                                        plaintext.size(),
                                        nonce, kNonceLen,
                                        nullptr, 0,
                                        plaintext.data(), ct,
                                        kTagLen, tag);
    if (rc != 0) {
        ESP_LOGW(kTag, "gcm encrypt failed: -0x%04x", -rc);
        return tl::unexpected{Error::CryptoBadKey};
    }
    return wire;
}

tl::expected<std::vector<std::uint8_t>, Error> Session::decrypt(std::span<const std::uint8_t> wire)
{
    if (!established_) {
        return tl::unexpected{Error::CryptoNotReady};
    }
    if (wire.size() < kNonceLen + kTagLen) {
        return tl::unexpected{Error::CryptoAuth};
    }
    const std::size_t pt_len = wire.size() - kNonceLen - kTagLen;
    const std::uint8_t* nonce = wire.data();
    const std::uint8_t* ct = wire.data() + kNonceLen;
    const std::uint8_t* tag = wire.data() + kNonceLen + pt_len;

    std::vector<std::uint8_t> plaintext(pt_len);
    int rc = mbedtls_gcm_auth_decrypt(&gcm_, pt_len,
                                       nonce, kNonceLen,
                                       nullptr, 0,
                                       tag, kTagLen,
                                       ct, plaintext.data());
    if (rc != 0) {
        ESP_LOGW(kTag, "gcm decrypt/auth failed: -0x%04x", -rc);
        return tl::unexpected{Error::CryptoAuth};
    }
    return plaintext;
}

void Session::reset()
{
    mbedtls_gcm_free(&gcm_);
    mbedtls_gcm_init(&gcm_);
    mbedtls_platform_zeroize(device_pub_.data(), device_pub_.size());
    mbedtls_platform_zeroize(device_priv_.data(), device_priv_.size());
    mbedtls_platform_zeroize(aes_key_.data(), aes_key_.size());
    keypair_ready_ = false;
    established_ = false;
}

} // namespace stackchan::config::crypto
