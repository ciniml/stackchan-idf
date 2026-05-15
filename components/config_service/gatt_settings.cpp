// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "gatt_settings.hpp"
#include "config_store.hpp"

#include <array>
#include <optional>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_uuid.h>
#include <host/ble_hs_mbuf.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

namespace stackchan::config::gatt {

namespace {

constexpr const char* kTag = "cfg-gatt";

// --- UUIDs — 128-bit, stored little-endian (byte[0] = LSB of the 128-bit value).
// Service: e3f0a000-7b1c-4d2a-9e6f-2c5a8d4b1f00
// SSID:    e3f0a001-...  Pass: e3f0a002-...  Key: e3f0a003-...
// Apply:   e3f0a004-...  Status: e3f0a005-...

static const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x00, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kSsidUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x01, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kPassUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x02, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kApiKeyUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x03, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kApplyUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x04, 0xa0, 0xf0, 0xe3);
static const ble_uuid128_t kStatusUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x05, 0xa0, 0xf0, 0xe3);

// --- Mutable state guarded by g_mutex ---
static SemaphoreHandle_t g_mutex = nullptr;

struct StagingBuffer {
    std::optional<std::string> ssid, password, api_key;
};

static DeviceConfig g_active;
static StagingBuffer g_staging;
static bool g_wifi_connected = false;
static uint16_t g_status_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_status_subscribed = false;

// Val handles written by NimBLE during GATT registration.
static uint16_t g_ssid_handle = 0;
static uint16_t g_pass_handle = 0;
static uint16_t g_key_handle = 0;
static uint16_t g_apply_handle = 0;
static uint16_t g_status_handle = 0;

// One-shot timer fires ~200 ms after Apply to allow ATT response to flush.
static esp_timer_handle_t g_restart_timer = nullptr;

// --- Helpers ---

std::array<uint8_t, 2> compute_status_locked()
{
    uint8_t flags = 0;
    if (!g_active.wifi_ssid.empty()) flags |= 0x01;
    if (!g_active.wifi_password.empty()) flags |= 0x02;
    if (!g_active.openai_api_key.empty()) flags |= 0x04;
    return {flags, g_wifi_connected ? uint8_t{1} : uint8_t{0}};
}

void restart_cb(void* /*arg*/)
{
    ESP_LOGI(kTag, "restarting now");
    esp_restart();
}

// --- GATT access callback ---

static int gatt_access_cb(uint16_t /*conn_handle*/, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt* ctxt, void* /*arg*/)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == g_ssid_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const std::string ssid = g_active.wifi_ssid;
            xSemaphoreGive(g_mutex);
            int rc = os_mbuf_append(ctxt->om, ssid.data(), ssid.size());
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (attr_handle == g_status_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            auto st = compute_status_locked();
            xSemaphoreGive(g_mutex);
            int rc = os_mbuf_append(ctxt->om, st.data(), st.size());
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        std::array<char, 257> buf{};
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf.data(),
                                      static_cast<uint16_t>(buf.size() - 1), &out_len);
        if (rc != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;

        if (attr_handle == g_ssid_handle) {
            if (out_len > 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(buf.data(), out_len);
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.ssid = std::move(val);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_pass_handle) {
            if (out_len > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(buf.data(), out_len);
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.password = std::move(val);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_key_handle) {
            if (out_len > 256) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            std::string val(buf.data(), out_len);
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_staging.api_key = std::move(val);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_apply_handle) {
            if (out_len == 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

            xSemaphoreTake(g_mutex, portMAX_DELAY);
            DeviceConfig merged = g_active;
            if (g_staging.ssid) merged.wifi_ssid = *g_staging.ssid;
            if (g_staging.password) merged.wifi_password = *g_staging.password;
            if (g_staging.api_key) merged.openai_api_key = *g_staging.api_key;
            xSemaphoreGive(g_mutex);

            auto result = store::save(merged);
            if (!result) {
                ESP_LOGE(kTag, "NVS save failed on Apply");
                return BLE_ATT_ERR_UNLIKELY;
            }
            ESP_LOGI(kTag, "config saved, scheduling restart in 200 ms");

            if (g_restart_timer == nullptr) {
                esp_timer_create_args_t args{};
                args.callback = restart_cb;
                args.name = "ble_restart";
                esp_timer_create(&args, &g_restart_timer);
            }
            esp_timer_start_once(g_restart_timer, 200'000); // 200 ms
            return 0;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

// --- GATT service table ---
// These arrays live in static storage for the lifetime of the GATT server.

// Field order must match struct ble_gatt_chr_def: uuid, access_cb, arg,
// descriptors, flags, min_key_size, val_handle — C++ designated initializers
// must be in declaration order.
//
// NOTE: encryption (Just Works pairing) is currently DISABLED — the link is
// plaintext. The originally-planned _READ_ENC / _WRITE_ENC flags were removed
// because NimBLE Just Works pairing didn't complete cleanly with Web Bluetooth
// in our testing. Anyone in BLE range of an unconfigured device can write the
// settings. Acceptable v1 trade-off; revisit with persistent bonding later.
static ble_gatt_chr_def kChrs[] = {
    {
        .uuid = &kSsidUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ssid_handle,
    },
    {
        .uuid = &kPassUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_pass_handle,
    },
    {
        .uuid = &kApiKeyUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_key_handle,
    },
    {
        .uuid = &kApplyUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_apply_handle,
    },
    {
        .uuid = &kStatusUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_status_handle,
    },
    {} // terminator: uuid = nullptr
};

static ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = kChrs,
    },
    {} // terminator: type = BLE_GATT_SVC_TYPE_END (0)
};

} // namespace

void init(const DeviceConfig& active)
{
    g_mutex = xSemaphoreCreateMutex();
    g_active = active;

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
    }
}

uint16_t status_val_handle()
{
    return g_status_handle;
}

void set_subscribe(uint16_t conn_handle, bool subscribed)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_status_conn_handle = conn_handle;
    g_status_subscribed = subscribed;
    xSemaphoreGive(g_mutex);
    ESP_LOGD(kTag, "Status CCCD: conn=%d subscribed=%d", conn_handle, subscribed ? 1 : 0);
}

void set_wifi_connected(bool connected)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_wifi_connected = connected;
    auto st = compute_status_locked();
    const bool subscribed = g_status_subscribed;
    const uint16_t conn_h = g_status_conn_handle;
    const uint16_t val_h = g_status_handle;
    xSemaphoreGive(g_mutex);

    if (subscribed && conn_h != BLE_HS_CONN_HANDLE_NONE && val_h != 0) {
        struct os_mbuf* om = ble_hs_mbuf_from_flat(st.data(), st.size());
        if (om != nullptr) {
            ble_gatts_notify_custom(conn_h, val_h, om);
        }
    }
}

} // namespace stackchan::config::gatt
