// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <wifi_config_service/wifi_config_service.hpp>

#include "http_handlers.hpp"

#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mdns.h>

namespace stackchan::wifi_config {

namespace {

constexpr const char* kTag = "cfg-wifi";

bool g_started = false;
httpd_handle_t g_server = nullptr;
char g_hostname[32] = "stackchan";

// Build the mDNS hostname. If the operator set DeviceConfig.device_name we
// sanitize that to RFC-1123 (lowercase alphanumerics + single-hyphen runs, no
// leading/trailing hyphen, ≤24 chars) so it can serve as the .local label.
// Otherwise we fall back to the Wi-Fi STA MAC lower 3 bytes — the BLE
// advertising name (config_service) uses the SAME MAC bytes, so a central
// can derive the .local hostname from the BLE name (tools/settings.html does
// exactly that). STA is the ESP32 base MAC and is what the router shows, so
// the hostname suffix matches the device's visible Wi-Fi MAC.
//
// (Both sides MUST stay on ESP_MAC_WIFI_STA when falling back — they
// previously diverged, BLE on ESP_MAC_BT = base + 2, which made the
// settings-page link unresolvable.)
void compose_hostname(const std::string& operator_name)
{
    if (!operator_name.empty()) {
        std::size_t out = 0;
        bool last_hyphen = false;
        for (char c : operator_name) {
            if (out + 1 >= sizeof(g_hostname)) break;
            // ASCII alnum: keep as lowercase. Non-alnum collapses to a single
            // hyphen (skipping leading and consecutive runs).
            if ((c >= 'A' && c <= 'Z')) c = static_cast<char>(c - 'A' + 'a');
            const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (alnum) {
                g_hostname[out++] = c;
                last_hyphen = false;
            } else if (out > 0 && !last_hyphen) {
                g_hostname[out++] = '-';
                last_hyphen = true;
            }
        }
        // Trim trailing hyphen so we never publish "foo-.local".
        while (out > 0 && g_hostname[out - 1] == '-') --out;
        if (out > 0) {
            g_hostname[out] = '\0';
            return;
        }
        // Empty after sanitization (e.g. operator_name was all symbols) →
        // fall through to the MAC fallback.
    }
    std::uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(g_hostname, sizeof(g_hostname),
                  "stackchan-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

tl::expected<void, Error> start_mdns()
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "mdns_init: %s", esp_err_to_name(err));
        return tl::unexpected(Error::MdnsInit);
    }

    mdns_hostname_set(g_hostname);
    mdns_instance_name_set("Stack-chan config");

    // Plain HTTP on 80. TLS turned out too heavy on internal RAM (each
    // active TLS session was contending with the conversation task's
    // mbedtls / Wi-Fi AES DMA allocations). The settings page lives on
    // the home LAN so the trust model is "anyone on the LAN can talk to
    // it" — same as a router admin page.
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    mdns_service_add(nullptr, "_stackchan-config", "_tcp", 80, nullptr, 0);

    // Live audio receiver (main/wifi_audio.cpp): RTP on udp/6970. Advertise
    // the port + the wire format so a sender can discover where/how to push.
    // Keep the port in sync with kPort in main/wifi_audio.cpp.
    mdns_service_add(nullptr, "_stackchan-audio", "_udp", 6970, nullptr, 0);
    // udp/6970 carries L16 or μ-law (codec from RTP PT: 0 → PCMU/8000 G.711
    // μ-law e.g. OBS, else → L16/48000 ffmpeg/gst). AAC (MPEG4-GENERIC) shares
    // the dynamic PT with L16, so it has its own port (6972), surfaced here.
    mdns_txt_item_t audio_txt[] = {
        {"proto", "rtp"},
        {"codec", "L16,PCMU"},
        {"ch", "1"},
        {"aac_port", "6972"},
    };
    mdns_service_txt_set("_stackchan-audio", "_udp", audio_txt,
                         sizeof(audio_txt) / sizeof(audio_txt[0]));

    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc != nullptr) {
        mdns_txt_item_t txt[] = {
            {"version", desc->version},
            {"path", "/"},
        };
        mdns_service_txt_set("_http", "_tcp", txt, sizeof(txt) / sizeof(txt[0]));
        mdns_service_txt_set("_stackchan-config", "_tcp", txt, sizeof(txt) / sizeof(txt[0]));
    }

    ESP_LOGI(kTag, "mDNS published as %s.local", g_hostname);
    return {};
}

tl::expected<httpd_handle_t, Error> start_http_server()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    // Wide enough to hold all the /api/* + /mcp/* routes plus headroom for
    // future growth. register_handlers() currently registers 64 routes
    // (grep '    add(server,' http_handlers.cpp); a stale 48 here silently
    // dropped the last 3 (/mcp/balloon, /mcp/state, /mcp/events) with
    // "no slots left". esp_http_server allocates the slot table at
    // httpd_start, so going wider just consumes a few hundred bytes of
    // internal RAM; the runtime cost is the slot count we actually register,
    // not the cap. add() now ESP_LOGEs on registration failure so a future
    // count/cap drift is loud instead of silent.
    cfg.max_uri_handlers = 72;
    // Stack MUST live in internal RAM: the OTA control handler calls
    // esp_ota_begin → esp_partition_mmap, which disables CPU cache while
    // remapping flash. PSRAM is cached, so a PSRAM-resident stack becomes
    // unreadable during the cache-disable window and the kernel asserts
    // (cache_utils.c:152 esp_task_stack_is_sane_cache_disabled). Same
    // constraint applies to NVS writes triggered by /api/apply.
    //
    // 16 KiB (was 6): two synchronous, stack-hungry operations run on THIS
    // task and can't get a big enough stack any other way:
    //  (1) POST /api/hmm-voice → hmm_voice::store → the hts_engine model
    //      parser (HTS_ModelSet_load), which nests ~1 KiB HTS_MAXBUFLEN token
    //      buffers several frames deep (ModelSet → Model → Tree/Question).
    //      On the old 6 KiB stack it overflowed and corrupted the adjacent
    //      httpd session struct (crash surfaced one request later in
    //      httpd_resp_set_hdr).
    //  (2) POST /api/hmm-voice/fetch + GET /api/voices → voice_fetch does the
    //      mbedTLS handshake (~10 KiB) to GitHub Pages HERE. A separate worker
    //      task would be cleaner, but internal RAM is fragmented at runtime
    //      (largest free block ~10 KiB) so a 16 KiB worker stack can't be
    //      allocated on demand — whereas this httpd stack is allocated once at
    //      httpd_start, before fragmentation, so the contiguous block IS
    //      available. The two ops never overlap (single httpd task), so 16 KiB
    //      covers max(TLS, parse), not the sum.
    cfg.stack_size = 16384;
    cfg.lru_purge_enable = true;
    // 2 sockets: SSE long-poll (held open by the Channel adapter) + one
    // regular slot for /api/*, /mcp/say, /mcp/state, etc. esp_http_server
    // allocates ~4-8 KB of internal RAM per socket, so each extra slot eats
    // contiguous space that conversation_task needs for its 3 × 8 KB segment
    // buffers (see conversation_task.cpp's seg_buf_ alloc — fragmentation
    // there is what bricked the conv-task init at boot when this was 3).
    // lru_purge_enable means the older socket is reclaimed if both are busy
    // and a third request arrives.
    cfg.max_open_sockets = 2;

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start: %s", esp_err_to_name(err));
        return tl::unexpected(Error::HttpServerStart);
    }
    ESP_LOGI(kTag, "HTTP server listening on :%d", cfg.server_port);
    return server;
}

// Done on a worker task because mDNS init pulls in a few KB of stack we
// don't want eating into the sys_evt task. Even though plain HTTP is much
// lighter than TLS, this keeps the structure consistent and gives us
// margin for future additions.
void init_task(void* arg)
{
    const auto* current = static_cast<const config::DeviceConfig*>(arg);

    if (auto r = start_mdns(); !r) {
        ESP_LOGE(kTag, "start_mdns failed: %d", static_cast<int>(r.error()));
        vTaskDelete(nullptr);
        return;
    }

    auto server = start_http_server();
    if (!server) {
        ESP_LOGE(kTag, "start_http_server failed: %d", static_cast<int>(server.error()));
        vTaskDelete(nullptr);
        return;
    }
    g_server = *server;
    http::register_handlers(g_server, *current);

    // Belt-and-braces seed of the STA-connected flag. notify_wifi_connected()
    // (from IP_EVENT_STA_GOT_IP) can fire ~4 s before we reach this point;
    // g_wifi_connected being atomic means that early notification is no longer
    // dropped, but if the boot sequence is ever reordered so the notification
    // predates the flag's existence entirely — or is missed for any other
    // reason — GOT_IP does NOT re-fire, so we would stay stuck at false. Read
    // the current STA IP directly from esp_netif here and seed the flag if we
    // already have a lease; this makes the flag self-healing regardless of the
    // notify/init ordering.
    if (esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            http::set_wifi_connected(true);
        }
    }

    vTaskDelete(nullptr);
}

} // namespace

tl::expected<void, Error> start(const config::DeviceConfig& current)
{
    if (g_started) {
        return tl::unexpected(Error::AlreadyStarted);
    }

    compose_hostname(current.device_name);

    g_started = true;
    BaseType_t ok = xTaskCreate(init_task, "cfg-wifi-init", 6 * 1024,
                                const_cast<void*>(static_cast<const void*>(&current)),
                                tskIDLE_PRIORITY + 3, nullptr);
    if (ok != pdPASS) {
        g_started = false;
        ESP_LOGE(kTag, "xTaskCreate(cfg-wifi-init) failed");
        return tl::unexpected(Error::HttpServerStart);
    }
    return {};
}

httpd_handle_t handle()
{
    return g_server;
}

void notify_wifi_connected(bool connected)
{
    if (!g_started) return;
    http::set_wifi_connected(connected);
}

void set_provisioning_mode(bool active)
{
    // Allow the call before g_started: the http layer guards on its own
    // mutex, and a flag set before server start is read back correctly
    // once the first handler call comes in.
    http::set_provisioning_mode(active);
}

void set_battery(int millivolts, int milliamps, int percent)
{
    if (!g_started) return;
    http::set_battery(millivolts, milliamps, percent);
}

void set_settings_hooks(const config::SettingsHooks& hooks)
{
    http::set_servo_range_mode_sink(hooks.servo_range_mode);
    http::set_servo_positions_getter(hooks.servo_positions);
    http::set_audio_metrics_getter(hooks.audio_metrics);
    http::set_led_state_getter(hooks.led_state_get);
    http::set_led_state_sink(hooks.led_state_set);
    http::set_mic_lip_gain_getter(hooks.mic_lip_gain_get);
    http::set_mic_lip_gain_sink(hooks.mic_lip_gain_set);
    http::set_speaker_volume_getter(hooks.speaker_volume_get);
    http::set_speaker_volume_sink(hooks.speaker_volume_set);
    http::set_board_kind(hooks.board_kind);
}

void set_servo_range_mode_sink(config::ServoRangeModeSink sink)
{
    http::set_servo_range_mode_sink(sink);
}

void set_servo_positions_getter(config::ServoPositionsGetter getter)
{
    http::set_servo_positions_getter(getter);
}

void set_audio_metrics_getter(config::AudioMetricsJsonGetter getter)
{
    http::set_audio_metrics_getter(getter);
}

void set_led_state_getter(config::LedStateGetter getter)
{
    http::set_led_state_getter(getter);
}

void set_led_state_sink(config::LedStateSink sink)
{
    http::set_led_state_sink(sink);
}

void set_mic_lip_gain_getter(config::MicLipGainGetter getter)
{
    http::set_mic_lip_gain_getter(getter);
}

void set_mic_lip_gain_sink(config::MicLipGainSink sink)
{
    http::set_mic_lip_gain_sink(sink);
}

void set_speaker_volume_getter(config::SpeakerVolumeGetter getter)
{
    http::set_speaker_volume_getter(getter);
}

void set_speaker_volume_sink(config::SpeakerVolumeSink sink)
{
    http::set_speaker_volume_sink(sink);
}

void set_jtts_say_kana_sink(config::JttsSayKanaSink sink)
{
    http::set_jtts_say_kana_sink(sink);
}

void set_board_kind(std::uint8_t kind)
{
    http::set_board_kind(kind);
}

void set_avatar_bytecode_sink(AvatarBytecodeSink sink)
{
    http::set_avatar_bytecode_sink(std::move(sink));
}

void set_voice_db_sink(VoiceDbSink sink)
{
    http::set_voice_db_sink(std::move(sink));
}

void set_voice_db_status_getter(VoiceDbStatusGetter getter)
{
    http::set_voice_db_status_getter(std::move(getter));
}

void set_hmm_voice_sink(HmmVoiceSink sink)
{
    http::set_hmm_voice_sink(std::move(sink));
}

void set_hmm_voice_status_getter(HmmVoiceStatusGetter getter)
{
    http::set_hmm_voice_status_getter(std::move(getter));
}

void set_camera_capture_sink(CameraCaptureSink sink)
{
    http::set_camera_capture_sink(std::move(sink));
}

void set_camera_reg_sink(CameraRegSink sink)
{
    http::set_camera_reg_sink(std::move(sink));
}

void set_mcp_say_kana_sink(McpSayKanaSink sink)
{
    http::set_mcp_say_kana_sink(std::move(sink));
}

void set_lt_config_sink(LtConfigSink sink)
{
    http::set_lt_config_sink(std::move(sink));
}
void set_mcp_expression_sink(McpExpressionSink sink)
{
    http::set_mcp_expression_sink(std::move(sink));
}
void set_mcp_balloon_sink(McpBalloonSink sink)
{
    http::set_mcp_balloon_sink(std::move(sink));
}

} // namespace stackchan::wifi_config
