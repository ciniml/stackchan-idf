// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "espnow_remote/espnow_remote.hpp"

#include <algorithm>
#include <cstdint>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include <espnow.h>
#include <espnow_storage.h>
#include <espnow_utils.h>

namespace stackchan::espnow {

namespace {

constexpr const char* kTag = "espnow-rx";

// 受信フィルタ用に自機 ID とハンドラを保持 (esp-now の C コールバックは
// ユーザ ポインタを渡せないため、この翻訳単位内の静的で受け渡す)。
std::uint8_t g_receiver_id = 1;
PoseHandler g_handler;

// espressif/esp-now の受信コールバック。ライブラリがフレーム ヘッダを剥がした
// 8 バイト ペイロードが渡る。target-id フィルタを通ったものだけ g_handler へ。
esp_err_t on_recv(std::uint8_t* src, void* data, size_t size, wifi_pkt_rx_ctrl_t* rx)
{
    if (size < 8) {
        ESP_LOGD(kTag, "RX size=%d (<8) ignored", static_cast<int>(size));
        return ESP_OK;
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::uint8_t tid = p[0];
    if (tid != 0 && tid != g_receiver_id) {
        return ESP_OK;  // 他機宛
    }
    Pose pose;
    pose.target_id = tid;
    pose.yaw = static_cast<std::int16_t>(p[1] | (p[2] << 8));
    pose.pitch = static_cast<std::int16_t>(p[3] | (p[4] << 8));
    pose.speed = static_cast<std::int16_t>(p[5] | (p[6] << 8));
    pose.laser = p[7];
    pose.rssi = rx ? static_cast<std::int8_t>(rx->rssi) : 0;
    if (g_handler) {
        g_handler(pose);
    }
    return ESP_OK;
}

tl::expected<void, esp_err_t> wifi_init_fixed_channel(int ch)
{
    // esp_netif / event loop は M5.begin 等で既に上がっている可能性がある。
    // 二重初期化の ESP_ERR_INVALID_STATE は許容する。
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return tl::unexpected(e);
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return tl::unexpected(e);
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) return tl::unexpected(e);
    if ((e = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return tl::unexpected(e);
    if ((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return tl::unexpected(e);
    if ((e = esp_wifi_start()) != ESP_OK) return tl::unexpected(e);
    // 混雑モードで一旦ロックしてから信道固定 (M5Stack hal_espnow と同手順)。
    // AP には connect しない — 固定チャネルを保つため。
    esp_wifi_set_promiscuous(true);
    if ((e = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE)) != ESP_OK) return tl::unexpected(e);
    esp_wifi_set_promiscuous(false);
    return {};
}

}  // namespace

tl::expected<void, esp_err_t> start(const ReceiverConfig& cfg, PoseHandler on_pose)
{
    const int ch = std::clamp<int>(cfg.channel, 1, 13);
    g_receiver_id = cfg.receiver_id;
    g_handler = std::move(on_pose);

    ESP_LOGI(kTag, "ESP-NOW remote receiver: channel=%d receiver_id=%u", ch, g_receiver_id);

    espnow_storage_init();
    if (auto r = wifi_init_fixed_channel(ch); !r) {
        ESP_LOGE(kTag, "wifi init failed: %s", esp_err_to_name(r.error()));
        return r;
    }

    // M5Stack 公式 hal_espnow と同一設定 (生パケット互換: forward 無効・data 受信有効)。
    espnow_config_t ec = ESPNOW_INIT_CONFIG_DEFAULT();
    ec.forward_enable = false;
    ec.forward_switch_channel = false;
    ec.send_retry_num = 5;
    ec.receive_enable.forward = false;
    ec.receive_enable.data = true;
    if (esp_err_t e = espnow_init(&ec); e != ESP_OK) {
        ESP_LOGE(kTag, "espnow_init failed: %s", esp_err_to_name(e));
        return tl::unexpected(e);
    }
    espnow_set_config_for_data_type(ESPNOW_DATA_TYPE_DATA, true, on_recv);

    std::uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    ESP_LOGI(kTag, "listening as " MACSTR " on channel %d (id %u)", MAC2STR(mac), ch, g_receiver_id);
    return {};
}

}  // namespace stackchan::espnow
