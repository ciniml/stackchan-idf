// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "dis.hpp"

#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_log.h>

#include <host/ble_gatt.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>

namespace stackchan::config::dis {

namespace {

constexpr const char* kTag = "cfg-dis";

// Compile-time identity. Manufacturer / Model / Hardware are fixed for this
// firmware family; Firmware Revision and Software Revision are filled at
// init() time from the running app's description.
constexpr const char* kManufacturer = "Kenta IDA";
constexpr const char* kModelNumber = "Stack-chan CoreS3";
constexpr const char* kHardwareRev = "CoreS3 + SCS0009";

// 0x180A — Device Information Service (Bluetooth SIG-assigned).
static const ble_uuid16_t kDisUuid = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t kMfrUuid = BLE_UUID16_INIT(0x2A29);     // Manufacturer Name
static const ble_uuid16_t kModelUuid = BLE_UUID16_INIT(0x2A24);   // Model Number
static const ble_uuid16_t kHwRevUuid = BLE_UUID16_INIT(0x2A27);   // Hardware Revision
static const ble_uuid16_t kFwRevUuid = BLE_UUID16_INIT(0x2A26);   // Firmware Revision
static const ble_uuid16_t kSwRevUuid = BLE_UUID16_INIT(0x2A28);   // Software Revision

// Filled at init() time. `version` carries `git describe --always --tags
// --dirty`, `idf_ver` carries the ESP-IDF version that built this binary.
static char g_fw_rev[40] = {};  // esp_app_desc_t::version is 32 bytes + prefix
static char g_sw_rev[40] = {};  // "esp-idf " + esp_app_desc_t::idf_ver (32)

static int append(struct os_mbuf* om, const char* s)
{
    return os_mbuf_append(om, s, std::strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_cb(uint16_t /*conn_handle*/, uint16_t /*attr_handle*/,
                      struct ble_gatt_access_ctxt* ctxt, void* /*arg*/)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    const ble_uuid_t* uuid = ctxt->chr->uuid;
    if (ble_uuid_cmp(uuid, &kMfrUuid.u) == 0)   return append(ctxt->om, kManufacturer);
    if (ble_uuid_cmp(uuid, &kModelUuid.u) == 0) return append(ctxt->om, kModelNumber);
    if (ble_uuid_cmp(uuid, &kHwRevUuid.u) == 0) return append(ctxt->om, kHardwareRev);
    if (ble_uuid_cmp(uuid, &kFwRevUuid.u) == 0) return append(ctxt->om, g_fw_rev);
    if (ble_uuid_cmp(uuid, &kSwRevUuid.u) == 0) return append(ctxt->om, g_sw_rev);
    return BLE_ATT_ERR_ATTR_NOT_FOUND;
}

static const ble_gatt_chr_def kChrs[] = {
    {
        .uuid = &kMfrUuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &kModelUuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &kHwRevUuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &kFwRevUuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &kSwRevUuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {} // terminator
};

static const ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kDisUuid.u,
        .characteristics = kChrs,
    },
    {} // terminator
};

} // namespace

void init()
{
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc != nullptr) {
        std::snprintf(g_fw_rev, sizeof(g_fw_rev), "%s", desc->version);
        std::snprintf(g_sw_rev, sizeof(g_sw_rev), "esp-idf %s", desc->idf_ver);
    } else {
        std::snprintf(g_fw_rev, sizeof(g_fw_rev), "unknown");
        std::snprintf(g_sw_rev, sizeof(g_sw_rev), "unknown");
    }
    ESP_LOGI(kTag, "DIS: %s / %s / %s / fw=%s / sw=%s",
             kManufacturer, kModelNumber, kHardwareRev, g_fw_rev, g_sw_rev);

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

} // namespace stackchan::config::dis
