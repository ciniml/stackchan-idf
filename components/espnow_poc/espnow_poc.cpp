// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// [ESP-NOW PoC] M5Stack 公式 Stack-chan (github.com/m5stack/StackChan) の ESP-NOW
// リモコン/デバイス間同期プロトコルと互換の送受信を、espressif/esp-now コンポーネントで
// 実機疎通する。目的: (1) esp-now が ESP-IDF 5.5 でビルド/init できるか、(2) 8 バイト
// ペイロード [id, yaw i16, pitch i16, speed i16, laser u8] を送受信できるか。
// 詳細は docs/espnow-m5stackchan-protocol-research.md。

#include "sdkconfig.h"

#if defined(CONFIG_STACKCHAN_ESPNOW_POC)

#include "espnow_poc/espnow_poc.hpp"

#include <cstdint>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <espnow.h>
#include <espnow_storage.h>
#include <espnow_utils.h>

namespace {
constexpr const char* kTag = "espnow-poc";
constexpr int kChannel = 1;         // 送受で一致必須 (M5Stack 側メニューと合わせる)
constexpr std::uint8_t kReceiverId = 1;

// 受信コールバック: ライブラリがヘッダを剥がした 8 バイト ペイロードが渡る。
esp_err_t on_recv(std::uint8_t* src, void* data, size_t size, wifi_pkt_rx_ctrl_t* rx) {
    if (size >= 8) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        const std::uint8_t tid = p[0];
        const std::int16_t yaw = static_cast<std::int16_t>(p[1] | (p[2] << 8));
        const std::int16_t pitch = static_cast<std::int16_t>(p[3] | (p[4] << 8));
        const std::int16_t speed = static_cast<std::int16_t>(p[5] | (p[6] << 8));
        const std::uint8_t laser = p[7];
        const bool mine = (tid == 0 || tid == kReceiverId);
        ESP_LOGI(kTag, "RX [" MACSTR "] rssi=%d id=%u%s yaw=%d pitch=%d speed=%d laser=%u",
                 MAC2STR(src), rx ? rx->rssi : 0, tid, mine ? "(me)" : "", yaw, pitch, speed, laser);
    } else {
        ESP_LOGI(kTag, "RX size=%d (expected >=8)", static_cast<int>(size));
    }
    return ESP_OK;
}

void wifi_init_fixed_channel(int ch) {
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 混雑モードで一旦ロックしてから信道固定 (M5Stack hal_espnow と同手順)。
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
}
}  // namespace

namespace stackchan::app {

void espnow_poc_run() {
    ESP_LOGI(kTag, "ESP-NOW PoC start (channel %d, receiver id %u)", kChannel, kReceiverId);

    espnow_storage_init();
    wifi_init_fixed_channel(kChannel);

    // M5Stack 公式と同一設定 (Arduino/生パケット互換のため forward 無効, data 受信有効)。
    espnow_config_t ec = ESPNOW_INIT_CONFIG_DEFAULT();
    ec.forward_enable = false;
    ec.forward_switch_channel = false;
    ec.send_retry_num = 5;
    ec.receive_enable.forward = false;
    ec.receive_enable.data = true;
    espnow_init(&ec);
    espnow_set_config_for_data_type(ESPNOW_DATA_TYPE_DATA, true, on_recv);

    std::uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    ESP_LOGI(kTag, "MAC " MACSTR " — listening; also broadcasting a test sweep", MAC2STR(mac));

    // 送信疎通確認: broadcast(target-id 0)で yaw を掃引した 8B パケットを 500ms 毎に送出。
    for (int n = 0;; ++n) {
        std::uint8_t pkt[8];
        const std::int16_t yaw = static_cast<std::int16_t>(((n % 256) - 128) * 10);  // -1280..1270
        const std::int16_t pitch = 450;
        const std::int16_t speed = 600;
        pkt[0] = 0;  // broadcast
        pkt[1] = yaw & 0xFF;
        pkt[2] = (yaw >> 8) & 0xFF;
        pkt[3] = pitch & 0xFF;
        pkt[4] = (pitch >> 8) & 0xFF;
        pkt[5] = speed & 0xFF;
        pkt[6] = (speed >> 8) & 0xFF;
        pkt[7] = 0;
        espnow_frame_head_t fh = ESPNOW_FRAME_CONFIG_DEFAULT();
        const esp_err_t r =
            espnow_send(ESPNOW_DATA_TYPE_DATA, ESPNOW_ADDR_BROADCAST, pkt, sizeof(pkt), &fh, portMAX_DELAY);
        if (n % 20 == 0) ESP_LOGI(kTag, "TX #%d yaw=%d ret=%s", n, yaw, esp_err_to_name(r));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace stackchan::app

#else  // !CONFIG_STACKCHAN_ESPNOW_POC

#include "espnow_poc/espnow_poc.hpp"
namespace stackchan::app {
void espnow_poc_run() {}
}  // namespace stackchan::app

#endif
