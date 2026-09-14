// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "ble.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#include <esp_log.h>
#include <esp_mac.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_store.h>
#include <host/ble_uuid.h>
#include <mbedtls/sha256.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include <config_service/crypto.hpp>
#include <config_service/ota.hpp>

// NimBLE の bond store 初期化 (ヘッダ非公開、examples と同じく前方宣言)。
extern "C" void ble_store_config_init(void);

namespace stackchan::recovery::ble {

namespace {

constexpr const char* kTag = "rcv-ble";

// Main (components/config_service/gatt_settings.cpp) と同じ UUID。
// e3f0a000-7b1c-4d2a-9e6f-2c5a8d4b1f00 (service)
const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x00, 0xa0, 0xf0, 0xe3);
// KeyExchange: e3f0a006 — plaintext。READ = device X25519 pub、WRITE = peer pub。
const ble_uuid128_t kKeyExchangeUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x06, 0xa0, 0xf0, 0xe3);
// OtaControl: e3f0a009 — encrypted JSON (begin/end/abort)、READ = status。
const ble_uuid128_t kOtaControlUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x09, 0xa0, 0xf0, 0xe3);
// OtaData: e3f0a00a — encrypted WRITE-only chunk。
const ble_uuid128_t kOtaDataUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x0a, 0xa0, 0xf0, 0xe3);

constexpr std::size_t kMaxControlBytes = 960;
constexpr std::size_t kMaxOtaChunkBytes = 768;

std::uint16_t g_kx_handle = 0;
std::uint16_t g_ota_ctrl_handle = 0;
std::uint16_t g_ota_data_handle = 0;
std::uint8_t g_own_addr_type = 0;
char g_device_name[32] = "Stackchan";
char g_configured_name[32] = {};
std::atomic<bool> g_connected{false};

// GATT access callback は NimBLE host task 上でのみ走り、Recovery には
// session を触る別タスクが無いので mutex は不要。
config::crypto::Session g_session;

int gap_event_cb(ble_gap_event* event, void* arg);

void start_advertising()
{
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kSvcUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gap_adv_set_fields: %d", rc);
        return;
    }

    ble_hs_adv_fields rsp{};
    rsp.name = reinterpret_cast<const std::uint8_t*>(g_device_name);
    rsp.name_len = static_cast<std::uint8_t>(std::strlen(g_device_name));
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(kTag, "ble_gap_adv_rsp_set_fields: %d", rc);
    }

    ble_gap_adv_params adv{};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &adv, gap_event_cb, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(kTag, "ble_gap_adv_start: %d", rc);
    } else {
        ESP_LOGI(kTag, "advertising as \"%s\"", g_device_name);
    }
}

int gap_event_cb(ble_gap_event* event, void* /*arg*/)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            const std::uint16_t conn = event->connect.conn_handle;
            ESP_LOGI(kTag, "connected: handle=%d", conn);
            g_connected.store(true, std::memory_order_relaxed);
            // OTA スループット: DLE + 2M PHY + 短い接続間隔 (Main と同じ)。
            (void)ble_hs_hci_util_set_data_len(conn, 251, 2120);
            (void)ble_gap_set_prefered_le_phy(conn, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
            ble_gap_upd_params params{};
            params.itvl_min = 6;
            params.itvl_max = 12;
            params.latency = 0;
            params.supervision_timeout = 400;
            (void)ble_gap_update_params(conn, &params);
        } else if (event->connect.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGW(kTag, "connect failed: status=%d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(kTag, "disconnected: reason=%d", event->disconnect.reason);
        g_connected.store(false, std::memory_order_relaxed);
        // 途中までのイメージが起動可能になることは無いようにする。
        config::ota::abort_update();
        g_session.reset();
        start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(kTag, "MTU: conn=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

bool append_encrypted(os_mbuf* om, std::span<const std::uint8_t> plain)
{
    auto enc = g_session.encrypt(plain);
    if (!enc) return false;
    return os_mbuf_append(om, enc->data(), enc->size()) == 0;
}

int gatt_access_cb(std::uint16_t /*conn_handle*/, std::uint16_t attr_handle,
                   ble_gatt_access_ctxt* ctxt, void* /*arg*/)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == g_kx_handle) {
            auto pub = g_session.ensure_device_keypair();
            if (!pub) {
                ESP_LOGW(kTag, "ensure_device_keypair failed");
                return BLE_ATT_ERR_UNLIKELY;
            }
            return os_mbuf_append(ctxt->om, pub->data(), pub->size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (attr_handle == g_ota_ctrl_handle) {
            if (!g_session.is_established()) return BLE_ATT_ERR_UNLIKELY;
            const std::string json = config::ota::status_json();
            const bool ok = append_encrypted(
                ctxt->om, {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()});
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // host task 上でのみ使う共有スクラッチ (スタックを圧迫しないよう static)。
        static std::array<std::uint8_t, 1024> buf;
        std::uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf.data(), static_cast<std::uint16_t>(buf.size()), &out_len);
        if (rc != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;

        if (attr_handle == g_kx_handle) {
            if (out_len != 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            auto result = g_session.complete_handshake(std::span<const std::uint8_t, 32>{buf.data(), 32});
            if (!result) {
                ESP_LOGW(kTag, "complete_handshake failed: %d", static_cast<int>(result.error()));
                return result.error() == config::Error::CryptoNotReady ? BLE_ATT_ERR_UNLIKELY
                                                                       : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            return 0;
        }

        if (!g_session.is_established()) return BLE_ATT_ERR_UNLIKELY;
        auto pt = g_session.decrypt({buf.data(), out_len});
        if (!pt) return BLE_ATT_ERR_UNLIKELY;

        if (attr_handle == g_ota_ctrl_handle) {
            if (pt->size() > kMaxControlBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string cmd(reinterpret_cast<const char*>(pt->data()), pt->size());
            (void)config::ota::handle_control_command(cmd);
            return 0;
        }
        if (attr_handle == g_ota_data_handle) {
            if (pt->size() > kMaxOtaChunkBytes) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            (void)config::ota::handle_data_chunk({pt->data(), pt->size()});
            return 0;
        }
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

ble_gatt_chr_def kChrs[] = {
    {
        .uuid = &kKeyExchangeUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_kx_handle,
    },
    {
        .uuid = &kOtaControlUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ota_ctrl_handle,
    },
    {
        .uuid = &kOtaDataUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &g_ota_data_handle,
    },
    {} // terminator: uuid = nullptr
};

const ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = kChrs,
    },
    {} // terminator: type = BLE_GATT_SVC_TYPE_END (0)
};

void on_sync()
{
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_hs_id_infer_auto: %d", rc);
        return;
    }
    if (g_configured_name[0] != '\0') {
        std::snprintf(g_device_name, sizeof(g_device_name), "%s", g_configured_name);
    } else {
        std::uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        std::snprintf(g_device_name, sizeof(g_device_name), "Stackchan-%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    ble_svc_gap_device_name_set(g_device_name);
    ESP_LOGI(kTag, "BLE host ready, name=%s", g_device_name);
    start_advertising();
}

void on_reset(int reason)
{
    ESP_LOGE(kTag, "NimBLE host reset: reason=%d", reason);
}

void host_task(void* /*param*/)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

} // namespace

void start(const std::string& device_name, const std::string& auth_password)
{
    if (!device_name.empty()) {
        std::snprintf(g_configured_name, sizeof(g_configured_name), "%s", device_name.c_str());
    }

    if (!auth_password.empty()) {
        std::array<std::uint8_t, 32> hash{};
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(auth_password.data()),
                       auth_password.size(), hash.data(), 0);
        g_session.set_hkdf_salt(std::span<const std::uint8_t>{hash});
        ESP_LOGI(kTag, "BLE auth gate ON");
    } else {
        g_session.set_hkdf_salt({});
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(kSvcs);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gatts_count_cfg: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(kSvcs);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gatts_add_svcs: %d", rc);
        return;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

bool connected()
{
    return g_connected.load(std::memory_order_relaxed);
}

} // namespace stackchan::recovery::ble
