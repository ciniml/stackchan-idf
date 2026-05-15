// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/config_service.hpp>
#include "config_store.hpp"
#include "gatt_settings.hpp"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_store.h>
#include <host/ble_uuid.h>
#include <services/gap/ble_svc_gap.h>

// ESP-IDF's NimBLE port does not auto-initialise the bond store backend;
// it must be called explicitly (see the bleprph example). Without it every
// store access returns ENOTSUP and pairing aborts. No public header declares
// it, so forward-declare it here.
extern "C" void ble_store_config_init(void);

namespace stackchan::config {

namespace {

constexpr const char* kTag = "cfg-svc";

static uint8_t g_own_addr_type = 0;
static char g_device_name[32] = "Stackchan";

// Service UUID for the advertising payload (little-endian).
// e3f0a000-7b1c-4d2a-9e6f-2c5a8d4b1f00
static const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0x00, 0x1f, 0x4b, 0x8d, 0x5a, 0x2c, 0x6f, 0x9e,
    0x2a, 0x4d, 0x1c, 0x7b, 0x00, 0xa0, 0xf0, 0xe3);

// Forward declaration: gap_event_cb is referenced by start_advertising.
static int gap_event_cb(struct ble_gap_event* event, void* arg);

static void start_advertising()
{
    // Main adv payload: flags (3 B) + 128-bit UUID (18 B) = 21 B, fits in 31 B.
    struct ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kSvcUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gap_adv_set_fields: %d", rc);
        return;
    }

    // Scan response: complete device name (Web Bluetooth uses active scan).
    struct ble_hs_adv_fields rsp{};
    rsp.name = reinterpret_cast<const uint8_t*>(g_device_name);
    rsp.name_len = static_cast<uint8_t>(std::strlen(g_device_name));
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(kTag, "ble_gap_adv_rsp_set_fields: %d", rc);
    }

    struct ble_gap_adv_params adv_params{};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(kTag, "ble_gap_adv_start: %d", rc);
    } else {
        ESP_LOGI(kTag, "advertising as \"%s\"", g_device_name);
    }
}

static int gap_event_cb(struct ble_gap_event* event, void* /*arg*/)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        // NimBLE re-fires BLE_GAP_EVENT_CONNECT after "Read Remote Features /
        // Version Complete" with that HCI command's status (ble_gap.c:2865),
        // even though the connection itself is already up. Treat as a true
        // connect failure only when no connection handle is associated.
        if (event->connect.status == 0) {
            ESP_LOGI(kTag, "connected: handle=%d", event->connect.conn_handle);
        } else if (event->connect.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGW(kTag, "connect failed: status=%d", event->connect.status);
            start_advertising();
        } else {
            // Post-connect secondary status event — connection is still up.
            ESP_LOGD(kTag, "post-connect status: handle=%d status=%d",
                     event->connect.conn_handle, event->connect.status);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(kTag, "disconnected: reason=%d", event->disconnect.reason);
        gatt::set_subscribe(BLE_HS_CONN_HANDLE_NONE, false);
        start_advertising();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(kTag, "enc_change: conn=%d status=%d",
                 event->enc_change.conn_handle, event->enc_change.status);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == gatt::status_val_handle()) {
            gatt::set_subscribe(event->subscribe.conn_handle,
                                event->subscribe.cur_notify != 0);
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(kTag, "MTU: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        break;
    }
    return 0;
}

static void on_sync()
{
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_hs_id_infer_auto: %d", rc);
        return;
    }

    // Build device name from BT MAC address lower 3 bytes.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BT);
    std::snprintf(g_device_name, sizeof(g_device_name),
                  "Stackchan-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ble_svc_gap_device_name_set(g_device_name);

    ESP_LOGI(kTag, "BLE host ready, name=%s addr_type=%d", g_device_name, g_own_addr_type);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(kTag, "NimBLE host reset: reason=%d", reason);
}

static void nimble_host_task(void* /*param*/)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

} // namespace

DeviceConfig load()
{
    return store::load();
}

tl::expected<void, Error> start(const DeviceConfig& current)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init: %s", esp_err_to_name(err));
        return tl::unexpected(Error::NimbleInit);
    }

    // Pairing is currently disabled — characteristics carry no _ENC flags
    // (see gatt_settings.cpp). The SM config below sits dormant. Future:
    // re-enable Just Works pairing once the NimBLE / Web Bluetooth pairing
    // flow is sorted out (likely needs persistent bonding to be robust).
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    gatt::init(current);

    // Register the bond store backend (RAM-only — CONFIG_BT_NIMBLE_NVS_PERSIST
    // is off, so pairings never persist across reboots). Required for the
    // security manager to function.
    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);
    return {};
}

void notify_wifi_connected(bool connected)
{
    gatt::set_wifi_connected(connected);
}

} // namespace stackchan::config
