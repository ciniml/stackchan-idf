// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "http_handlers.hpp"
#include "release_ota.hpp"
#include "voice_fetch.hpp"

#include <avatar_vm/storage.hpp>
#include <config_service/config_service.hpp>
#include <config_service/config_store.hpp>
#include <config_service/ota.hpp>
#include <config_service/settings_registry.hpp>
#include <config_service/staged_config.hpp>
#include <wifi_config_service/mcp_events.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cJSON.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <mbedtls/base64.h>

// Embedded settings page (added by CMake EMBED_TXTFILES).
extern const char settings_wifi_html_start[] asm("_binary_settings_wifi_html_start");
extern const char settings_wifi_html_end[]   asm("_binary_settings_wifi_html_end");

namespace stackchan::wifi_config::http {

namespace {

constexpr const char* kTag = "cfg-http";

// Single mutex around staging buffers + the active config snapshot. The
// HTTP server runs a single worker task by default so contention is
// minimal — the lock is mainly to keep state consistent between writes
// from the worker and reads from notify_wifi_connected() (esp_event task).
SemaphoreHandle_t g_mutex = nullptr;


config::DeviceConfig g_active;
config::StagedConfig g_staging;

// [settings redesign 案C] Immediate-apply path. For settings that take effect
// at runtime (no reboot): update the live config (g_active becomes live for
// this key), persist just that key, and fire the generic change hook so the
// subsystem reflects it now. Replaces g_staging.set_* for Immediate settings.
// (Phase 1: HTTP only. BLE stays staged for these until Phase 2; a BLE Apply
// in the same session could still re-save the boot value — see the design memo
// §互換性 / Phase 3 g_active unification.)
void apply_immediate_num(const char* id, std::uint32_t v)
{
    const auto* d = config::registry::find(id);
    if (d == nullptr) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    d->num_set(g_active, v);
    const config::DeviceConfig snap = g_active;
    xSemaphoreGive(g_mutex);
    (void)config::store::save_one(*d, snap);
    config::notify_config_change(*d, snap);
}

// String 版。g_active の str フィールドへ書き、単一キー保存 + 通知。g_active の
// 読み手(require_auth / status / settings_get)は既に g_mutex 下で読むため、
// str の即時書き込みでもデータ競合しない。
void apply_immediate_str(const char* id, std::string value)
{
    const auto* d = config::registry::find(id);
    if (d == nullptr || d->str_member == nullptr) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_active.*(d->str_member) = std::move(value);
    const config::DeviceConfig snap = g_active;
    xSemaphoreGive(g_mutex);
    (void)config::store::save_one(*d, snap);
    config::notify_config_change(*d, snap);
}

// STA-connected flag. Deliberately std::atomic and NOT guarded by g_mutex:
// notify_wifi_connected(true) fires from the IP_EVENT_STA_GOT_IP handler,
// which lands during a ~4 s window BEFORE the cfg-wifi-init task has run
// register_handlers() / created g_mutex (measured: got-IP @12.7 s,
// HTTP-server-up @16.5 s). The old mutex-guarded setter dropped that
// notification on the floor (`if (g_mutex == nullptr) return;`) and GOT_IP
// never re-fires, so the flag stayed false forever. An atomic lets
// set_wifi_connected() record the state regardless of init ordering; the
// cfg-wifi-init task also seeds it from esp_netif once handlers register
// (belt-and-braces), so a future reorder of the boot sequence self-heals.
std::atomic<bool> g_wifi_connected{false};
// True while SoftAP provisioning is active — bypasses require_auth (physical
// AP button = implicit trust; iOS captive portals don't reliably carry the
// Basic auth prompt). Set/cleared from wifi_config::set_provisioning_mode.
// Atomic for the same init-ordering reason as g_wifi_connected above: when
// the stored SSID is gone, the FIRST wifi_config::start() happens on the
// AP-enable path and set_provisioning_mode(true) arrives before the
// cfg-wifi-init task has created g_mutex — the old mutex-guarded setter
// dropped it (`if (g_mutex == nullptr) return;`), leaving every request
// behind the Basic-auth gate (Android's captive WebView surfaces the 401
// as ERR_HTTP_RESPONSE_CODE_FAILURE; observed on HW).
std::atomic<bool> g_provisioning_mode{false};
// Cached battery snapshot served by /api/status. -1 mV / percent → unknown.
int g_battery_mv = -1;
int g_battery_ma = 0;
int g_battery_pct = -1;
esp_timer_handle_t g_restart_timer = nullptr;

// Servo range-setting mode integration. Sink is invoked on every POST to
// /api/servo-range-mode (and the cached active state is mirrored locally so
// /api/status can return it). Getter is consulted on /api/status to surface
// live present-positions for the capture UI.
config::ServoRangeModeSink g_servo_range_mode_sink = nullptr;
config::ServoPositionsGetter g_servo_positions_getter = nullptr;
config::AudioMetricsJsonGetter g_audio_metrics_getter = nullptr;
config::LedStateGetter g_led_state_getter = nullptr;
config::LedStateSink g_led_state_sink = nullptr;
config::MicLipGainGetter g_mic_lip_gain_getter = nullptr;
config::MicLipGainSink g_mic_lip_gain_sink = nullptr;
config::SpeakerVolumeGetter g_speaker_volume_getter = nullptr;
config::SpeakerVolumeSink g_speaker_volume_sink = nullptr;
config::JttsSayKanaSink g_jtts_say_sink = nullptr;
AvatarBytecodeSink g_avatar_bytecode_sink = nullptr;
VoiceDbSink g_voice_db_sink = nullptr;
VoiceDbStatusGetter g_voice_db_status_getter = nullptr;
HmmVoiceSink g_hmm_voice_sink = nullptr;
HmmVoiceStatusGetter g_hmm_voice_status_getter = nullptr;
CameraCaptureSink g_camera_capture_sink = nullptr;
CameraRegSink g_camera_reg_sink = nullptr;
McpSayKanaSink g_mcp_say_sink = nullptr;
LtConfigSink g_lt_config_sink = nullptr;
McpExpressionSink g_mcp_expression_sink = nullptr;
McpBalloonSink g_mcp_balloon_sink = nullptr;
bool g_servo_range_mode = false;
// Active /mcp/* bearer. NVS-resolved at register_handlers() time; empty →
// /mcp/* answers 404. Defined here (rather than next to the /mcp/* code
// block below) so the /api/status handler higher up the file can read
// has_mcp_token without a forward declaration.
std::string g_mcp_active_token;
// Board variant (mirrors board::BoardKind). Defaults to 0 = M5Base for
// compat — overwritten by main at boot.
std::uint8_t g_board_kind = 0;

// Plaintext caps mirror the BLE chr limits so the same DeviceConfig.cpp
// invariants hold whichever transport wrote the value.
constexpr std::size_t kMaxSsid = 32;
constexpr std::size_t kMaxPassword = 64;
constexpr std::size_t kMaxApiKey = 256;
constexpr std::size_t kMaxJttsConfig = 768;
constexpr std::size_t kMaxSystemPrompt = 2048;
constexpr std::size_t kMaxConvHeaders = 1024;
constexpr std::size_t kMaxOtaChunk = 16384; // HTTP can chunk much bigger than BLE
constexpr std::size_t kMaxBodyBytes = 32768;
constexpr std::size_t kMaxDeviceName = 24;
constexpr std::size_t kMaxAuthPassword = 64;

// Constant-time string comparison for secrets (passwords / tokens). Both the
// per-byte XOR accumulation and the length check are folded into a single
// running accumulator so the response timing leaks neither the matching
// prefix length nor the secret's length. Modelled on mbedtls's constant-time
// helpers: we always walk the full expected length, indexing the candidate
// modulo its own length (never out of bounds) so the loop count depends only
// on `expected`, and OR in the length delta at the end.
bool constant_time_equals(std::string_view a, std::string_view b)
{
    // Iterate over the longer of the two so a length mismatch cannot shorten
    // the loop; the length delta below is what actually rejects it.
    const std::size_t n = std::max(a.size(), b.size());
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < n; ++i) {
        // Index modulo each side's length so neither read runs past the end;
        // when sizes differ the length delta above already forces a mismatch.
        const unsigned char ca = a.empty() ? 0 : static_cast<unsigned char>(a[i % a.size()]);
        const unsigned char cb = b.empty() ? 0 : static_cast<unsigned char>(b[i % b.size()]);
        diff |= static_cast<unsigned>(ca ^ cb);
    }
    return diff == 0;
}

// --- HTTP Basic Auth gate ---
//
// Every public handler calls require_auth() at the top. When
// DeviceConfig.auth_password is empty (default / back-compat), the gate is a
// no-op. When set, the request must carry an Authorization: Basic header
// whose decoded password (everything after the first ':') matches; otherwise
// we 401 with WWW-Authenticate so the browser pops its native prompt. The
// username field is ignored — Stack-chan has no concept of multiple users.
bool require_auth(httpd_req_t* req)
{
    const bool ap_mode = g_provisioning_mode.load();
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const std::string expected = g_active.auth_password;
    xSemaphoreGive(g_mutex);
    // SoftAP provisioning: trust the user — they had to physically tap the
    // on-device "AP モード" button to get here, and iOS's captive-portal flow
    // doesn't carry the WWW-Authenticate prompt reliably.
    if (ap_mode) return true;
    if (expected.empty()) return true;

    char hdr[160];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr));
    if (err == ESP_OK) {
        constexpr const char* kPrefix = "Basic ";
        if (std::strncmp(hdr, kPrefix, 6) == 0) {
            // mbedtls_base64_decode accepts the strict alphabet (RFC 4648),
            // sufficient for what browsers send. olen is set even on
            // BUFFER_TOO_SMALL — we pre-size the destination to the upper
            // bound (input_len * 3 / 4).
            const char* b64 = hdr + 6;
            const std::size_t b64_len = std::strlen(b64);
            std::vector<unsigned char> dec(b64_len + 3, 0);
            std::size_t olen = 0;
            int rc = mbedtls_base64_decode(dec.data(), dec.size(), &olen,
                                            reinterpret_cast<const unsigned char*>(b64),
                                            b64_len);
            if (rc == 0) {
                std::string up(reinterpret_cast<const char*>(dec.data()), olen);
                auto colon = up.find(':');
                if (colon != std::string::npos) {
                    const std::string pwd = up.substr(colon + 1);
                    if (constant_time_equals(pwd, expected)) return true;
                }
            }
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    // Realm must be wrapped in literal quotes per RFC 7617; "stackchan" is
    // shown verbatim in the browser prompt.
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"stackchan\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, nullptr, 0);
    return false;
}

// --- Body helpers ---

esp_err_t read_body(httpd_req_t* req, std::vector<std::uint8_t>& out, std::size_t max_bytes)
{
    const int len = req->content_len;
    if (len < 0 || static_cast<std::size_t>(len) > max_bytes) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, nullptr, 0);
        return ESP_FAIL;
    }
    out.resize(static_cast<std::size_t>(len));
    std::size_t off = 0;
    while (off < out.size()) {
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(out.data() + off),
                                       out.size() - off);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, nullptr, 0);
            return ESP_FAIL;
        }
        off += static_cast<std::size_t>(got);
    }
    return ESP_OK;
}

esp_err_t read_body_str(httpd_req_t* req, std::string& out, std::size_t max_bytes)
{
    std::vector<std::uint8_t> raw;
    if (read_body(req, raw, max_bytes) != ESP_OK) return ESP_FAIL;
    out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    return ESP_OK;
}

esp_err_t send_json(httpd_req_t* req, const std::string& body)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t send_text(httpd_req_t* req, const std::string& body)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t send_bytes(httpd_req_t* req, const std::uint8_t* data, std::size_t len)
{
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char*>(data), len);
}

esp_err_t send_empty(httpd_req_t* req, const char* status = "204 No Content")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t send_error(httpd_req_t* req, const char* status, const char* msg = nullptr)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, msg, msg ? std::strlen(msg) : 0);
}

void restart_cb(void* /*arg*/)
{
    ESP_LOGI(kTag, "restarting now");
    esp_restart();
}

std::string current_wifi_ip()
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) return {};
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return {};
    if (info.ip.addr == 0) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return std::string(buf);
}

// --- /api/status returns a JSON snapshot of everything the page needs to
// render itself: flags, current SSID, provider, openai_enabled, jtts_config,
// firmware/idf versions, and current Wi-Fi IP. One round-trip instead of
// the ~7 BLE GATT reads the equivalent UI does. ---

std::string escape_json(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 4);
    for (char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

esp_err_t handle_status_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    const bool wifi_ok = g_wifi_connected.load();
    const bool prov_mode = g_provisioning_mode.load();
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const auto cfg = g_active;
    const int bat_mv = g_battery_mv;
    const int bat_ma = g_battery_ma;
    const int bat_pct = g_battery_pct;
    const bool range_mode = g_servo_range_mode;
    const bool has_camera = static_cast<bool>(g_camera_capture_sink);
    config::ServoPositionsGetter pos_getter = g_servo_positions_getter;
    const bool has_mcp = !g_mcp_active_token.empty();  // ロック下でスナップショット
    xSemaphoreGive(g_mutex);

    config::ServoPositionsView pos{-1, -1};
    if (pos_getter != nullptr) pos = pos_getter();

    const esp_app_desc_t* desc = esp_app_get_description();
    const std::string fw = desc ? desc->version : "unknown";
    const std::string idf_ver = desc ? desc->idf_ver : "unknown";
    const std::string ip = current_wifi_ip();

    std::string body = "{";
    body += "\"firmware\":\"" + escape_json(fw) + "\",";
    body += "\"idf\":\"" + escape_json(idf_ver) + "\",";
    body += "\"ip\":\"" + escape_json(ip) + "\",";
    body += "\"wifi_connected\":" + std::string(wifi_ok ? "true" : "false") + ",";
    body += "\"provisioning_mode\":" + std::string(prov_mode ? "true" : "false") + ",";
    body += "\"has_camera\":" + std::string(has_camera ? "true" : "false") + ",";
    body += "\"ssid\":\"" + escape_json(cfg.wifi_ssid) + "\",";
    body += "\"has_password\":" + std::string(cfg.wifi_password.empty() ? "false" : "true") + ",";
    body += "\"has_openai_key\":" + std::string(cfg.openai_api_key.empty() ? "false" : "true") + ",";
    body += "\"has_gemini_key\":" + std::string(cfg.gemini_api_key.empty() ? "false" : "true") + ",";
    body += "\"xiaozhi_url\":\"" + escape_json(cfg.xiaozhi_url) + "\",";
    body += "\"has_xiaozhi_token\":" + std::string(cfg.xiaozhi_token.empty() ? "false" : "true") + ",";
    body += "\"openai_enabled\":" + std::string(cfg.openai_enabled ? "true" : "false") + ",";
    body += "\"rtp_audio_enabled\":" + std::string(cfg.rtp_audio_enabled ? "true" : "false") + ",";
    body += "\"jtts_idle_enabled\":" + std::string(cfg.jtts_idle_enabled ? "true" : "false") + ",";
    body += "\"battery_gauge_enabled\":" + std::string(cfg.battery_gauge_enabled ? "true" : "false") + ",";
    body += "\"startup_arpeggio_enabled\":" + std::string(cfg.startup_arpeggio_enabled ? "true" : "false") + ",";
    body += "\"servo_enabled\":" + std::string(cfg.servo_enabled ? "true" : "false") + ",";
    body += "\"led_mouth_sync_enabled\":" + std::string(cfg.led_mouth_sync_enabled ? "true" : "false") + ",";
    body += "\"operation_mode\":" + std::to_string(static_cast<int>(cfg.operation_mode)) + ",";
    body += "\"audio_output\":" + std::to_string(static_cast<int>(cfg.audio_output)) + ",";
    body += "\"lip_sync_mode\":" + std::to_string(static_cast<int>(cfg.lip_sync_mode)) + ",";
    body += "\"mic_lip_agc_enabled\":" + std::string(cfg.mic_lip_agc_enabled ? "true" : "false") + ",";
    body += "\"barge_in_enabled\":" + std::string(cfg.barge_in_enabled ? "true" : "false") + ",";
    body += "\"device_name\":\"" + escape_json(cfg.device_name) + "\",";
    body += "\"has_auth_password\":" + std::string(cfg.auth_password.empty() ? "false" : "true") + ",";
    // Token itself is never returned; the UI only needs to know whether the
    // /mcp/* API is reachable (= has a token).
    body += "\"has_mcp_token\":" + std::string(has_mcp ? "true" : "false") + ",";
    body += "\"provider\":" + std::to_string(static_cast<int>(cfg.provider)) + ",";
    body += "\"jtts_config\":\"" + escape_json(cfg.jtts_config_json) + "\",";
    body += "\"servo_limits\":\"" + escape_json(cfg.servo_limits_json) + "\",";
    body += "\"lt_config\":\"" + escape_json(cfg.lt_config_json) + "\",";
    body += "\"system_prompt\":\"" + escape_json(cfg.system_prompt) + "\",";
    body += "\"conv_extra_headers\":\"" + escape_json(cfg.conv_extra_headers) + "\",";
    body += "\"battery_mv\":" + std::to_string(bat_mv) + ",";
    body += "\"battery_ma\":" + std::to_string(bat_ma) + ",";
    body += "\"battery_pct\":" + std::to_string(bat_pct) + ",";
    body += "\"servo_range_mode\":" + std::string(range_mode ? "true" : "false") + ",";
    body += "\"servo_yaw_raw\":" + std::to_string(pos.yaw_raw) + ",";
    body += "\"servo_pitch_raw\":" + std::to_string(pos.pitch_raw) + ",";
    body += "\"board\":" + std::to_string(g_board_kind);
    body += "}";
    return send_json(req, body);
}

// --- Settings endpoints: each POST body is a plain UTF-8 string for the
// text fields, or a single byte (0/1) for the boolean/enum ones. The body
// is stored in g_staging and committed atomically by /api/apply. ---

esp_err_t handle_ssid_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxSsid) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("ssid", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_password_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxPassword) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("password", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_api_key_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxApiKey) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("api-key", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_gemini_api_key_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxApiKey) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("gemini-api-key", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_xiaozhi_url_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxApiKey) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("xiaozhi-url", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_xiaozhi_token_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxApiKey) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("xiaozhi-token", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_openai_enabled_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("openai-enabled", enabled ? 1 : 0);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_rtp_enabled_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("rtp-enabled", enabled ? 1 : 0);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_jtts_idle_enabled_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("jtts-idle-enabled", enabled ? 1 : 0);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_led_mouth_sync_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    apply_immediate_num("led-mouth-sync", enabled ? 1 : 0);  // 即時反映 (led_task)
    return send_empty(req);
}

esp_err_t handle_operation_mode_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    if (body.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "expected integer 0..4");
    }
    const int v = std::atoi(body.c_str());
    if (v < 0 || v > static_cast<int>(config::OperationMode::EspNowRemote)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "operation_mode out of range");
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("operation-mode", static_cast<std::uint32_t>(v));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_audio_output_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    if (body.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "expected integer 0..2");
    }
    const int v = std::atoi(body.c_str());
    if (v < 0 || v > static_cast<int>(config::AudioOutput::ModuleAudio)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "audio_output out of range");
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("audio-output", static_cast<std::uint32_t>(v));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_lip_sync_mode_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    if (body.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "expected integer 0..1");
    }
    const int v = std::atoi(body.c_str());
    if (v < 0 || v > static_cast<int>(config::LipSyncMode::LevelMeter)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "lip_sync_mode out of range");
    }
    apply_immediate_num("lip-sync-mode", static_cast<std::uint32_t>(v));  // 即時反映 (led_task)
    return send_empty(req);
}

esp_err_t handle_mic_lip_agc_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && body[0] == '1';
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("mic-lip-agc", enabled ? 1 : 0);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_barge_in_enabled_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    apply_immediate_num("barge-in", enabled ? 1 : 0);  // 即時反映 (SharedState barge_in_enabled)
    return send_empty(req);
}

esp_err_t handle_battery_gauge_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("battery-gauge", enabled ? 1 : 0);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_startup_arpeggio_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    // boot-only 設定: 実行時の反映先は無いが、staging+Apply+再起動を経ずに即永続化する。
    apply_immediate_num("startup-arpeggio", enabled ? 1 : 0);
    return send_empty(req);
}

esp_err_t handle_servo_enabled_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("servo-enabled", enabled ? 1 : 0);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_lt_config_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    // LT timekeeper config JSON. Applied live through the sink (demo_loop
    // re-parses off this task's stack) and staged so /api/apply persists it.
    std::string body;
    if (read_body_str(req, body, kMaxJttsConfig) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("lt-config", body);
    LtConfigSink sink = g_lt_config_sink;
    xSemaphoreGive(g_mutex);
    if (sink) sink(body);
    return send_empty(req);
}

esp_err_t handle_mcp_token_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    // The token can be anything up to 128 bytes; the firmware just stores it
    // verbatim and constant-time compares against the Authorization header.
    // An empty body explicitly clears the NVS token (next reboot → 404 on
    // /mcp/*, or falls back to the Kconfig build-time default if present).
    std::string body;
    if (read_body_str(req, body, 128) != ESP_OK) return ESP_OK;
    // 即時反映: MCP 認証キャッシュ g_mcp_active_token を live 更新(空 body で無効化)。
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_mcp_active_token = body;
    xSemaphoreGive(g_mutex);
    apply_immediate_str("mcp-token", std::move(body));
    return send_empty(req);
}

esp_err_t handle_provider_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const int v = body.empty() ? 0 : (body[0] - '0');
    config::Provider p = config::Provider::OpenAi;
    if (v == static_cast<int>(config::Provider::Gemini)) p = config::Provider::Gemini;
    else if (v == static_cast<int>(config::Provider::XiaoZhi)) p = config::Provider::XiaoZhi;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_num("provider", static_cast<std::uint32_t>(p));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_jtts_config_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxJttsConfig) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("jtts-config", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_servo_limits_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxJttsConfig) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("servo-limits", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_system_prompt_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxSystemPrompt) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("system-prompt", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_conv_headers_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxConvHeaders) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("conv-headers", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_servo_range_mode_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool on = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_servo_range_mode = on;
    config::ServoRangeModeSink sink = g_servo_range_mode_sink;
    xSemaphoreGive(g_mutex);
    if (sink != nullptr) sink(on);
    return send_empty(req);
}

esp_err_t handle_device_name_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxDeviceName) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.set_str("device-name", std::move(body));
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_auth_password_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxAuthPassword) != ESP_OK) return ESP_OK;
    // 即時反映: require_auth が g_active.auth_password を毎リクエスト参照するので
    // 次のリクエストから新パスワードが効く(再起動不要)。
    apply_immediate_str("auth-password", std::move(body));
    return send_empty(req);
}

// --- Batch settings API: one GET/POST pair over the whole registry. The
// per-key POST routes above remain for compatibility (existing settings
// pages and external clients); new clients can read and stage everything
// in two round-trips. ---

// GET /api/settings — every staged-domain setting (ApplyKind Staged/Both)
// from the active config, keyed by registry id. Secret rows come back as
// "has_<id>" booleans only. Live rows (LED / mic gain / speaker volume) are
// deliberately absent: g_active only knows their boot values, so their
// dedicated GET routes (which read the runtime state) stay authoritative.
esp_err_t handle_settings_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const auto cfg = g_active;
    xSemaphoreGive(g_mutex);

    using config::registry::ApplyKind;
    using config::registry::ValueType;
    std::string body = "{";
    bool first = true;
    for (const auto& d : config::registry::table()) {
        if (d.apply == ApplyKind::Live) continue;
        if (!first) body += ",";
        first = false;
        if (d.type == ValueType::Str) {
            const std::string& v = cfg.*(d.str_member);
            if (d.secret) {
                body += "\"has_" + std::string(d.id) + "\":" + (v.empty() ? "false" : "true");
            } else {
                body += "\"" + std::string(d.id) + "\":\"" + escape_json(v) + "\"";
            }
        } else if (d.type == ValueType::Bool) {
            body += "\"" + std::string(d.id) + "\":" + (d.num_get(cfg) != 0 ? "true" : "false");
        } else {
            body += "\"" + std::string(d.id) + "\":" + std::to_string(d.num_get(cfg));
        }
    }
    body += "}";
    return send_json(req, body);
}

// GET /api/reboot-required — JSON array of registry ids whose change only takes
// effect after a restart (ApplyKind::Staged). The settings UI reads this to
// badge those fields and to reboot on Apply only when one of them changed;
// everything else applies immediately via its per-key route. Additive endpoint
// so the existing /api/settings shape is untouched.
esp_err_t handle_reboot_required_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body = "[";
    bool first = true;
    for (const auto& d : config::registry::table()) {
        if (!config::registry::needs_reboot(d)) continue;
        if (!first) body += ",";
        first = false;
        body += "\"" + std::string(d.id) + "\"";
    }
    body += "]";
    return send_json(req, body);
}

// POST /api/settings — JSON object of {<registry id>: value, …} staged as a
// batch. Validated in full before anything is staged, so a 400 leaves
// staging untouched; /api/apply commits as usual. Unknown ids and live rows
// are rejected (typos must not silently no-op; live rows have their own
// immediate-apply routes).
esp_err_t handle_settings_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxBodyBytes) != ESP_OK) return ESP_OK;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "expected a JSON object");
    }

    using config::registry::ApplyKind;
    using config::registry::ValueType;
    // Pass 1: validate every member against its descriptor.
    std::string err;
    for (const cJSON* item = root->child; item != nullptr; item = item->next) {
        const char* id = item->string != nullptr ? item->string : "";
        const auto* d = config::registry::find(id);
        if (d == nullptr) {
            err = std::string("unknown setting: ") + id;
            break;
        }
        if (d->apply == ApplyKind::Live) {
            err = std::string(id) + ": live setting, use /api/" + id;
            break;
        }
        if (d->type == ValueType::Str) {
            if (!cJSON_IsString(item)) {
                err = std::string(id) + ": expected string";
                break;
            }
            if (std::strlen(item->valuestring) > d->max_len) {
                err = std::string(id) + ": longer than " + std::to_string(d->max_len) + " bytes";
                break;
            }
        } else if (d->type == ValueType::Bool) {
            if (!cJSON_IsBool(item) && !cJSON_IsNumber(item)) {
                err = std::string(id) + ": expected boolean";
                break;
            }
        } else {
            if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
                (d->max_value != 0 && item->valuedouble > d->max_value)) {
                err = std::string(id) + ": expected integer 0.." + std::to_string(d->max_value);
                break;
            }
        }
    }
    if (!err.empty()) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", err.c_str());
    }

    // Pass 2: stage the whole batch under one lock.
    LtConfigSink lt_sink = nullptr;
    std::string lt_json;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    for (const cJSON* item = root->child; item != nullptr; item = item->next) {
        const auto* d = config::registry::find(item->string);
        if (d->type == ValueType::Str) {
            g_staging.set_str(*d, item->valuestring);
            // lt-config is ApplyKind::Both: mirror the per-key route and fire
            // the live sink so the timekeeper updates without waiting for the
            // Apply reboot.
            if (std::strcmp(d->id, "lt-config") == 0 && g_lt_config_sink != nullptr) {
                lt_sink = g_lt_config_sink;
                lt_json = item->valuestring;
            }
        } else if (d->type == ValueType::Bool) {
            g_staging.set_num(*d, cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valuedouble != 0) ? 1 : 0);
        } else {
            g_staging.set_num(*d, static_cast<std::uint32_t>(item->valuedouble));
        }
    }
    xSemaphoreGive(g_mutex);
    cJSON_Delete(root);
    if (lt_sink != nullptr) lt_sink(lt_json);
    return send_empty(req);
}

esp_err_t handle_apply_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    config::DeviceConfig merged = g_active;
    g_staging.merge_into(merged);
    xSemaphoreGive(g_mutex);

    auto result = config::store::save(merged);
    if (!result) {
        ESP_LOGE(kTag, "NVS save failed on Apply");
        return send_error(req, "500 Internal Server Error", "save failed");
    }
    ESP_LOGI(kTag, "config saved via HTTP, scheduling restart in 200 ms");

    if (g_restart_timer == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = restart_cb;
        args.name = "wifi_restart";
        esp_timer_create(&args, &g_restart_timer);
    }
    esp_timer_start_once(g_restart_timer, 200'000);
    return send_empty(req);
}

// --- OTA — JSON command + binary chunk POST. Both go through
// config::ota which is shared with the BLE service. ---

esp_err_t handle_ota_status_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    return send_json(req, config::ota::status_json());
}

esp_err_t handle_ota_control_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxJttsConfig) != ESP_OK) return ESP_OK;
    // An abort during a release-fetch needs to also pull the rug out from
    // the HTTPS read loop — otherwise the worker keeps streaming bytes into
    // a now-Idle ota state until the download completes. Forward the abort
    // to the worker; release_ota::request_abort is a no-op if no fetch is
    // running, so this is safe to call for every control command.
    if (body.find("\"abort\"") != std::string::npos) {
        release_ota::request_abort();
    }
    return send_json(req, config::ota::handle_control_command(body));
}

esp_err_t handle_ota_data_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::vector<std::uint8_t> body;
    if (read_body(req, body, kMaxOtaChunk) != ESP_OK) return ESP_OK;
    return send_json(req, config::ota::handle_data_chunk({body.data(), body.size()}));
}

// GET /api/release/versions
//
// Device-side proxy for GitHub Pages `versions.json`. Same JSON the desktop
// web-flasher UI consumes, forwarded via the device's own STA link so the
// on-device settings page can populate a version dropdown even when the
// browser side has no direct internet access (AP-mode iPhone — DNS hijack
// blocks the iPhone from resolving github.com, but the device's STA does
// not go through the hijacked DNS).
//
// Requires STA to be up. When STA is down this returns 502 Bad Gateway;
// the UI should fall back to a manual tag input.
esp_err_t handle_release_versions_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (!release_ota::fetch_versions_json(body)) {
        return send_error(req, "502 Bad Gateway",
                          "versions.json fetch failed (STA down?)");
    }
    // Body is already JSON — passthrough as-is with the JSON content-type
    // header that send_json sets.
    return send_json(req, body);
}

// POST /api/ota/release  — body: {"tag":"vX.Y.Z"}
//
// Kicks off a worker that fetches the per-board stackchan_idf.bin staged
// on GitHub Pages and streams it straight into esp_ota_write. Progress is
// reported through the *same* /api/ota/status endpoint as the browser
// upload path so the UI doesn't need a separate poller. Returns 200 +
// "{queued:true}" once the worker is spawned (the actual download takes
// 30–60 s); the UI polls /api/ota/status until phase == done, then waits
// for the reboot to land.
//
// Requires STA to be up (the device needs its own internet connection to
// reach GitHub Pages — the AP interface has no upstream). When STA is
// down the worker fails on http_client_open and /api/ota/status flips to
// idle.
esp_err_t handle_ota_release_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;

    // Release-fetch pulls the firmware over the device's STA uplink. In AP
    // (provisioning) mode STA is not associated, so the download can never
    // succeed — reject up front instead of spawning a worker that only fails
    // later on http_client_open. Same source of truth as /api/status.
    const bool sta_connected = g_wifi_connected.load();
    if (!sta_connected) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, R"({"ok":false,"error":"sta not connected"})");
    }

    std::string body;
    // Tags are short (≤ 32 chars per release_ota::tag_looks_safe), so a
    // 256 B body cap is generous.
    if (read_body_str(req, body, 256) != ESP_OK) return ESP_OK;

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return send_error(req, "400 Bad Request", "bad json");
    }
    const cJSON* tag = cJSON_GetObjectItemCaseSensitive(root, "tag");
    if (!cJSON_IsString(tag) || tag->valuestring == nullptr) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "tag required");
    }
    const std::string tag_str = tag->valuestring;
    cJSON_Delete(root);

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const std::uint8_t board = g_board_kind;
    xSemaphoreGive(g_mutex);

    auto r = release_ota::start(tag_str, board);
    if (!r) {
        const char* msg = "?";
        switch (r.error()) {
        case release_ota::StartError::AlreadyRunning:     msg = "already running"; break;
        case release_ota::StartError::BadTag:             msg = "bad tag";          break;
        case release_ota::StartError::UnknownBoard:       msg = "unknown board";    break;
        case release_ota::StartError::WorkerSpawnFailed:  msg = "spawn failed";     break;
        }
        return send_error(req, "400 Bad Request", msg);
    }
    return send_json(req,
                     R"({"ok":true,"queued":true})");
}

// GET /api/camera/capture — one-shot photo for the settings page.
//
// Response on success: a raw row-major frame with the dimensions in
// X-Frame-Width / X-Frame-Height and the pixel encoding in X-Frame-Format
// ("gray8" = 1 B/px, "rgb565be" = 2 B/px big-endian — QVGA colour is
// 153 600 B). Raw-over-HTTP avoids a JPEG encoder on the device: ~150 KB
// over LAN Wi-Fi is ~200 ms, and the page renders it via canvas
// putImageData.
//
// 404 = no camera on this board (sink never registered); 503 = camera
// currently unavailable (QR scan holds the driver, low memory, sensor
// fault). The sink itself blocks a few hundred ms for AGC settle.
esp_err_t handle_camera_capture_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    CameraCaptureSink sink = g_camera_capture_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) {
        return send_error(req, "404 Not Found", "no camera on this board");
    }

    // ?fmt=raw → raw Bayer mosaic (ISP bypass); ?test=colorbar → sensor
    // test pattern. Absent/other values fall through to the normal photo.
    CameraCaptureOptions options{};
    {
        char query[64];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[16];
            if (httpd_query_key_value(query, "fmt", val, sizeof(val)) == ESP_OK &&
                std::strcmp(val, "raw") == 0) {
                options.raw_bayer = true;
            }
            if (httpd_query_key_value(query, "test", val, sizeof(val)) == ESP_OK &&
                std::strcmp(val, "colorbar") == 0) {
                options.colorbar = true;
            }
        }
    }

    std::vector<std::uint8_t> frame;
    std::size_t w = 0, h = 0;
    std::string format = "gray8";
    if (!sink(options, frame, w, h, format) || frame.empty()) {
        return send_error(req, "503 Service Unavailable",
                          "capture failed (camera busy or sensor error)");
    }

    char wbuf[12], hbuf[12];
    std::snprintf(wbuf, sizeof(wbuf), "%u", static_cast<unsigned>(w));
    std::snprintf(hbuf, sizeof(hbuf), "%u", static_cast<unsigned>(h));
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Frame-Width", wbuf);
    httpd_resp_set_hdr(req, "X-Frame-Height", hbuf);
    httpd_resp_set_hdr(req, "X-Frame-Format", format.c_str());
    // Expose the custom headers to fetch() — without this the browser hides
    // them from cross-check reads even same-origin in some UA versions.
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers",
                       "X-Frame-Width, X-Frame-Height, X-Frame-Format");
    httpd_resp_send(req, reinterpret_cast<const char*>(frame.data()),
                    static_cast<ssize_t>(frame.size()));
    return ESP_OK;
}

// GET/POST /api/camera/reg — raw GC0308 register access for interactive
// colour tuning against a test chart. Query-string only (no body — keeps
// parsing trivial): page + reg select the register; POST additionally takes
// value and writes it. Numbers accept 0x-prefixed hex or decimal. Response
// is JSON {"page":P,"reg":R,"value":V} with the value read back (GET) or
// as written (POST).
esp_err_t handle_camera_reg(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    CameraRegSink sink = g_camera_reg_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) {
        return send_error(req, "404 Not Found", "no camera on this board");
    }

    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "missing query (page, reg[, value])");
    }
    // strtoul with base 0 handles both "0x24" and "36".
    auto parse_u8 = [&](const char* key, std::uint8_t& out) -> bool {
        char val[16];
        if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) return false;
        char* endp = nullptr;
        const unsigned long v = std::strtoul(val, &endp, 0);
        if (endp == val || *endp != '\0' || v > 0xff) return false;
        out = static_cast<std::uint8_t>(v);
        return true;
    };

    std::uint8_t page = 0, reg = 0, value = 0;
    if (!parse_u8("page", page) || !parse_u8("reg", reg)) {
        return send_error(req, "400 Bad Request", "page/reg must be 0..255 (0x.. ok)");
    }
    const bool write = (req->method == HTTP_POST);
    if (write && !parse_u8("value", value)) {
        return send_error(req, "400 Bad Request", "POST needs value=0..255");
    }
    if (!sink(write, page, reg, value)) {
        return send_error(req, "503 Service Unavailable",
                          "register access failed (camera busy or bus error)");
    }

    char body[64];
    std::snprintf(body, sizeof(body), "{\"page\":%u,\"reg\":%u,\"value\":%u}",
                  static_cast<unsigned>(page), static_cast<unsigned>(reg),
                  static_cast<unsigned>(value));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// POST /api/avatar-dsl — body is a single complete `.avbc` (no chunking; HTTP
// can carry the whole bytecode in one request). Validates, persists to NVS,
// and applies live via the registered sink. Returns a JSON status. NVS write
// happens before sink invocation so a sink failure leaves the new face active
// after the next reboot anyway.
esp_err_t handle_avatar_dsl_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::vector<std::uint8_t> body;
    if (read_body(req, body, avatar_vm::storage::kMaxBytecodeBytes) != ESP_OK) return ESP_OK;

    auto save_r = avatar_vm::storage::save(body);
    if (!save_r) {
        ESP_LOGE(kTag, "avatar-dsl save: %s", avatar_vm::storage::to_string(save_r.error()));
        return send_error(req, "400 Bad Request",
                          avatar_vm::storage::to_string(save_r.error()));
    }

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    AvatarBytecodeSink sink = g_avatar_bytecode_sink;
    xSemaphoreGive(g_mutex);

    bool live_ok = true;
    if (sink) live_ok = sink(body.data(), body.size());

    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  R"({"ok":true,"saved":%u,"live":%s})",
                  static_cast<unsigned>(body.size()), live_ok ? "true" : "false");
    return send_json(req, buf);
}

// POST /api/avatar-dsl/reset — clears the NVS override and reapplies the
// firmware-embedded default face live.
esp_err_t handle_avatar_dsl_reset_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    auto r = avatar_vm::storage::clear();
    if (!r) {
        return send_error(req, "500 Internal Server Error",
                          avatar_vm::storage::to_string(r.error()));
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    AvatarBytecodeSink sink = g_avatar_bytecode_sink;
    xSemaphoreGive(g_mutex);
    // Passing length=0 is the documented "revert to default" signal.
    if (sink) (void)sink(nullptr, 0);
    return send_json(req, R"({"ok":true})");
}

// --- 音声 DB (.jvox) --------------------------------------------------------
// 単位連結 TTS の音声 DB。body は数百 KB になるので内部 RAM の vector では
// なく PSRAM に受ける。検証/保存/live ロードは sink (main/voice_db) 側。

esp_err_t handle_voice_db_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    VoiceDbSink sink = g_voice_db_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) return send_error(req, "503 Service Unavailable", "voice-db sink not ready");

    constexpr std::size_t kMax = 480 * 1024;
    const int len = req->content_len;
    if (len <= 0 || static_cast<std::size_t>(len) > kMax) {
        return send_error(req, "413 Payload Too Large", "voice DB must be 1..480 KiB");
    }
    std::uint8_t* buf =
        static_cast<std::uint8_t*>(heap_caps_malloc(static_cast<std::size_t>(len),
                                                    MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        return send_error(req, "507 Insufficient Storage", "no PSRAM for upload buffer");
    }
    std::size_t off = 0;
    while (off < static_cast<std::size_t>(len)) {
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(buf + off),
                                       static_cast<std::size_t>(len) - off);
        if (got <= 0) {
            heap_caps_free(buf);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, nullptr, 0);
            return ESP_OK;
        }
        off += static_cast<std::size_t>(got);
    }
    const char* err = sink(buf, static_cast<std::size_t>(len));
    heap_caps_free(buf);
    if (err != nullptr) {
        ESP_LOGE(kTag, "voice-db store: %s", err);
        return send_error(req, "400 Bad Request", err);
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    VoiceDbStatusGetter getter = g_voice_db_status_getter;
    xSemaphoreGive(g_mutex);
    const VoiceDbStatus st = getter ? getter() : VoiceDbStatus{};
    char body[96];
    std::snprintf(body, sizeof(body), R"({"ok":true,"units":%u,"stored":%u})",
                  static_cast<unsigned>(st.units), static_cast<unsigned>(st.stored_bytes));
    return send_json(req, body);
}

esp_err_t handle_voice_db_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    VoiceDbStatusGetter getter = g_voice_db_status_getter;
    xSemaphoreGive(g_mutex);
    const VoiceDbStatus st = getter ? getter() : VoiceDbStatus{};
    char body[112];
    std::snprintf(body, sizeof(body),
                  R"({"loaded":%s,"units":%u,"stored":%u})",
                  st.loaded ? "true" : "false", static_cast<unsigned>(st.units),
                  static_cast<unsigned>(st.stored_bytes));
    return send_json(req, body);
}

esp_err_t handle_voice_db_clear_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    VoiceDbSink sink = g_voice_db_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) return send_error(req, "503 Service Unavailable", "voice-db sink not ready");
    const char* err = sink(nullptr, 0);
    if (err != nullptr) return send_error(req, "500 Internal Server Error", err);
    return send_json(req, R"({"ok":true})");
}

// --- HMM ボイス (.htsvoice) -------------------------------------------------
// body は最大 ~2 MB。PSRAM に受けて sink (main/hmm_voice) が flash の voice
// パーティションへ書く。

esp_err_t handle_hmm_voice_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    HmmVoiceSink sink = g_hmm_voice_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) return send_error(req, "503 Service Unavailable", "hmm-voice sink not ready");

    constexpr std::size_t kMax = 4 * 1024 * 1024 - 16;  // voice パーティション上限
    const int len = req->content_len;
    if (len <= 0 || static_cast<std::size_t>(len) > kMax) {
        return send_error(req, "413 Payload Too Large", "htsvoice must be 1 B .. 4 MiB");
    }
    std::uint8_t* buf =
        static_cast<std::uint8_t*>(heap_caps_malloc(static_cast<std::size_t>(len),
                                                    MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        return send_error(req, "507 Insufficient Storage", "no PSRAM for upload buffer");
    }
    std::size_t off = 0;
    while (off < static_cast<std::size_t>(len)) {
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(buf + off),
                                       static_cast<std::size_t>(len) - off);
        if (got <= 0) {
            heap_caps_free(buf);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, nullptr, 0);
            return ESP_OK;
        }
        off += static_cast<std::size_t>(got);
    }
    const char* err = sink(buf, static_cast<std::size_t>(len));
    heap_caps_free(buf);
    if (err != nullptr) {
        ESP_LOGE(kTag, "hmm-voice store: %s", err);
        return send_error(req, "400 Bad Request", err);
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    HmmVoiceStatusGetter getter = g_hmm_voice_status_getter;
    xSemaphoreGive(g_mutex);
    const HmmVoiceStatus st = getter ? getter() : HmmVoiceStatus{};
    char body[96];
    std::snprintf(body, sizeof(body), R"({"ok":true,"stored":%u})",
                  static_cast<unsigned>(st.stored_bytes));
    return send_json(req, body);
}

esp_err_t handle_hmm_voice_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    HmmVoiceStatusGetter getter = g_hmm_voice_status_getter;
    xSemaphoreGive(g_mutex);
    const HmmVoiceStatus st = getter ? getter() : HmmVoiceStatus{};
    char body[128];
    std::snprintf(body, sizeof(body),
                  R"({"loaded":%s,"stored":%u,"capacity":%u})",
                  st.loaded ? "true" : "false", static_cast<unsigned>(st.stored_bytes),
                  static_cast<unsigned>(st.capacity));
    return send_json(req, body);
}

// GET /api/voices — Pages の voices.json マニフェストを HTTPS プロキシして返す
// (AP モードの iOS からもデバイス経由で取得できる)。STA が落ちていれば 502。
esp_err_t handle_voices_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (!voice_fetch::fetch_manifest(body)) {
        return send_error(req, "502 Bad Gateway", "manifest fetch failed (STA down?)");
    }
    return send_json(req, body.c_str());
}

// POST /api/hmm-voice/fetch — body = ボイス ID (例 "mei")。デバイスが Pages から
// .htsvoice を同期ダウンロードして voice パーティションへインストールする。
// httpd タスク (16 KiB 内部スタック) 上で mbedTLS + hts パースを走らせるので、
// このリクエストはダウンロード完了までブロックする (数〜数十秒)。UI は spinner
// を出して待つ。
esp_err_t handle_hmm_voice_fetch_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string id;
    if (read_body_str(req, id, 48) != ESP_OK) return ESP_OK;
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
    if (id.empty()) return send_error(req, "400 Bad Request", "empty voice id");

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    HmmVoiceSink sink = g_hmm_voice_sink;
    HmmVoiceStatusGetter getter = g_hmm_voice_status_getter;
    xSemaphoreGive(g_mutex);
    if (!sink) return send_error(req, "503 Service Unavailable", "hmm-voice sink not ready");

    const char* err = voice_fetch::fetch_and_install(
        id, [sink](const std::uint8_t* d, std::size_t n) { return sink(d, n); });
    if (err != nullptr) {
        ESP_LOGE(kTag, "voice fetch '%s': %s", id.c_str(), err);
        return send_error(req, "502 Bad Gateway", err);
    }
    const HmmVoiceStatus vst = getter ? getter() : HmmVoiceStatus{};
    char body[96];
    std::snprintf(body, sizeof(body), R"({"ok":true,"stored":%u})",
                  static_cast<unsigned>(vst.stored_bytes));
    return send_json(req, body);
}

esp_err_t handle_hmm_voice_clear_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    HmmVoiceSink sink = g_hmm_voice_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) return send_error(req, "503 Service Unavailable", "hmm-voice sink not ready");
    const char* err = sink(nullptr, 0);
    if (err != nullptr) return send_error(req, "500 Internal Server Error", err);
    return send_json(req, R"({"ok":true})");
}

// --- /mcp/* (Claude Code Channel adapter API, Bearer-auth) -------------
//
// Designed for exposure via Cloudflare Tunnel. Every request must carry
// `Authorization: Bearer <CONFIG_MCP_API_TOKEN>`. An empty Kconfig token
// disables the entire namespace (handlers respond 404), so the tunnel can
// stay wired up while the firmware ships with no live endpoint.
//
// Body convention: POST endpoints take a plain UTF-8 body (the value IS the
// body, no JSON wrapper — same pattern as the existing /api/* endpoints).
// GET /mcp/state returns JSON.

constexpr std::size_t kMaxMcpSayBytes = 1024;        // ~340 hiragana (UTF-8 3 B/char)
constexpr std::size_t kMaxMcpExpressionBytes = 16;   // longest name is "sleepy"
constexpr std::size_t kMaxMcpBalloonBytes = 1024;

bool mcp_auth_ok(httpd_req_t* req)
{
    // mcp-token は即時反映(handle_mcp_token_post が g_mutex 下で更新)するため、
    // ロック下でスナップショットしてから比較する(std::string の競合回避)。
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const std::string token = g_mcp_active_token;
    xSemaphoreGive(g_mutex);
    if (token.empty()) return false;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    constexpr const char* prefix = "Bearer ";
    constexpr std::size_t plen = 7;
    if (std::strncmp(hdr, prefix, plen) != 0) return false;
    // Constant-time compare on the token tail (length mismatch included).
    const char* tok = hdr + plen;
    return constant_time_equals(std::string_view(tok, std::strlen(tok)), token);
}

esp_err_t mcp_gate(httpd_req_t* req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const bool no_token = g_mcp_active_token.empty();
    xSemaphoreGive(g_mutex);
    // 404 (not 401) when the API is entirely disabled so the URL surface
    // is indistinguishable from an unrelated path.
    if (no_token) {
        return send_error(req, "404 Not Found");
    }
    if (!mcp_auth_ok(req)) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"stackchan\"");
        return send_error(req, "401 Unauthorized");
    }
    return ESP_OK;
}

esp_err_t handle_mcp_say_post(httpd_req_t* req)
{
    if (mcp_gate(req) != ESP_OK) return ESP_OK;
    std::string text;
    if (read_body_str(req, text, kMaxMcpSayBytes) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    McpSayKanaSink sink = g_mcp_say_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) return send_error(req, "503 Service Unavailable", "say sink not registered");
    sink(text);
    return send_json(req, R"({"ok":true})");
}

esp_err_t handle_mcp_expression_post(httpd_req_t* req)
{
    if (mcp_gate(req) != ESP_OK) return ESP_OK;
    std::string name;
    if (read_body_str(req, name, kMaxMcpExpressionBytes) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    McpExpressionSink sink = g_mcp_expression_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) return send_error(req, "503 Service Unavailable", "expression sink not registered");
    sink(name);
    return send_json(req, R"({"ok":true})");
}

esp_err_t handle_mcp_balloon_post(httpd_req_t* req)
{
    if (mcp_gate(req) != ESP_OK) return ESP_OK;
    std::string text;
    if (read_body_str(req, text, kMaxMcpBalloonBytes) != ESP_OK) return ESP_OK;
    // hold_ms is optional, in the query string: ?hold_ms=3000
    std::uint32_t hold_ms = 0;
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(q, "hold_ms", val, sizeof(val)) == ESP_OK) {
            hold_ms = static_cast<std::uint32_t>(std::strtoul(val, nullptr, 10));
        }
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    McpBalloonSink sink = g_mcp_balloon_sink;
    xSemaphoreGive(g_mutex);
    if (!sink) return send_error(req, "503 Service Unavailable", "balloon sink not registered");
    sink(text, hold_ms);
    return send_json(req, R"({"ok":true})");
}

esp_err_t handle_mcp_events_get(httpd_req_t* req)
{
    if (mcp_gate(req) != ESP_OK) return ESP_OK;
    // Hand the rest of the lifetime over to the events module. It writes the
    // SSE headers, streams frames, and tears down on disconnect / hand-off.
    return mcp_events::run_stream(req);
}

esp_err_t handle_mcp_state_get(httpd_req_t* req)
{
    if (mcp_gate(req) != ESP_OK) return ESP_OK;
    // /api/status returns the full settings + runtime state for the web UI;
    // /mcp/state is a Channel-oriented subset (no secrets, no chatbot
    // provider fields). Mirrors the keys Claude is most likely to query.
    const bool wifi_ok = g_wifi_connected.load();
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const int bat_mv = g_battery_mv;
    const int bat_pct = g_battery_pct;
    const std::uint8_t board = g_board_kind;
    xSemaphoreGive(g_mutex);

    const esp_app_desc_t* desc = esp_app_get_description();
    const std::string fw = desc ? desc->version : "unknown";
    const std::string ip = current_wifi_ip();

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  R"({"firmware":"%s","ip":"%s","wifi_connected":%s,"battery_mv":%d,"battery_pct":%d,"board":%u})",
                  escape_json(fw).c_str(), escape_json(ip).c_str(),
                  wifi_ok ? "true" : "false", bat_mv, bat_pct,
                  static_cast<unsigned>(board));
    return send_json(req, std::string{buf});
}

// --- Audio metrics ---

// GET /api/metrics/audio — last conversation-turn audio pipeline stats.
// Returns "{}" when no turn has finished yet (boot fresh / before first
// reply). Polled by an ops dashboard / phone client; does not require a
// session token.
esp_err_t handle_audio_metrics_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (g_audio_metrics_getter != nullptr) {
        body = g_audio_metrics_getter();
    } else {
        body = "{}";
    }
    return send_json(req, body);
}

// --- LED live state ---

// GET /api/led-state — current LED mode/colour/brightness/gradient period.
esp_err_t handle_led_state_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    config::LedState s{};
    if (g_led_state_getter != nullptr) s = g_led_state_getter();
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  R"({"mode":%u,"r":%u,"g":%u,"b":%u,"brightness":%u,"period_ds":%u})",
                  static_cast<unsigned>(s.mode), static_cast<unsigned>(s.r),
                  static_cast<unsigned>(s.g), static_cast<unsigned>(s.b),
                  static_cast<unsigned>(s.brightness),
                  static_cast<unsigned>(s.gradient_period_ds));
    return send_json(req, std::string{buf});
}

// POST /api/led-state — body is a small JSON with optional mode / r / g / b /
// brightness. Missing fields keep their current value. mode is 0..3
// (off/solid/breath/gradient). r/g/b are 0..255 each (gradient mode ignores
// them; the rainbow generator drives the strip itself). Returns the resulting
// state as JSON so the client can confirm the apply.
esp_err_t handle_led_state_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 256) != ESP_OK) return ESP_OK;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, R"({"error":"bad json"})");
    }
    config::LedStatePatch p{};
    auto pick = [root](const char* key) -> std::optional<std::uint8_t> {
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsNumber(n)) {
            const int v = n->valueint;
            if (v < 0 || v > 255) return std::nullopt;
            return static_cast<std::uint8_t>(v);
        }
        return std::nullopt;
    };
    p.mode = pick("mode");
    p.r = pick("r");
    p.g = pick("g");
    p.b = pick("b");
    p.brightness = pick("brightness");
    p.gradient_period_ds = pick("period_ds");
    cJSON_Delete(root);
    if (g_led_state_sink != nullptr) g_led_state_sink(p);
    return handle_led_state_get(req);
}

// --- Mic lip-sync calibration ---

// GET /api/mic-lip-gain — current input / output gain percentages.
esp_err_t handle_mic_lip_gain_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    config::MicLipGain g{100, 100};
    if (g_mic_lip_gain_getter != nullptr) g = g_mic_lip_gain_getter();
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  R"({"input_pct":%u,"output_pct":%u})",
                  static_cast<unsigned>(g.input_pct),
                  static_cast<unsigned>(g.output_pct));
    return send_json(req, std::string{buf});
}

// POST /api/mic-lip-gain — body is JSON `{"input_pct":..., "output_pct":...}`.
// Missing fields fall back to the current value. Sink clamps to 10..1000.
esp_err_t handle_mic_lip_gain_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 128) != ESP_OK) return ESP_OK;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, R"({"error":"bad json"})");
    }
    config::MicLipGain cur{100, 100};
    if (g_mic_lip_gain_getter != nullptr) cur = g_mic_lip_gain_getter();
    auto pick = [root, &cur](const char* key, std::uint16_t fallback) -> std::uint16_t {
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsNumber(n)) {
            const int v = n->valueint;
            if (v < 0) return 0;
            if (v > 65535) return 65535;
            return static_cast<std::uint16_t>(v);
        }
        return fallback;
    };
    config::MicLipGain g{};
    g.input_pct  = pick("input_pct",  cur.input_pct);
    g.output_pct = pick("output_pct", cur.output_pct);
    cJSON_Delete(root);
    if (g_mic_lip_gain_sink != nullptr) g_mic_lip_gain_sink(g);
    return handle_mic_lip_gain_get(req);
}

// --- Speaker volume ---

// GET /api/speaker-volume — current percent (0..200).
esp_err_t handle_speaker_volume_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::uint16_t pct = 100;
    if (g_speaker_volume_getter != nullptr) pct = g_speaker_volume_getter();
    char buf[40];
    std::snprintf(buf, sizeof(buf), R"({"pct":%u})", static_cast<unsigned>(pct));
    return send_json(req, std::string{buf});
}

// POST /api/speaker-volume — body is plain text integer 0..200.
esp_err_t handle_speaker_volume_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, 16) != ESP_OK) return ESP_OK;
    const int v = std::atoi(body.c_str());
    if (v < 0 || v > 200) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "speaker volume out of range (0..200)");
    }
    if (g_speaker_volume_sink != nullptr) {
        g_speaker_volume_sink(static_cast<std::uint16_t>(v));
    }
    return send_empty(req);
}

// POST /api/jtts-say — body is plain UTF-8 (kana) text. Test the
// jtts voice from the settings page. Same auth as the other /api/*
// endpoints (HTTP Basic via require_auth), so no MCP token needed —
// distinct from /mcp/say which is for external automation.
esp_err_t handle_jtts_say_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    constexpr std::size_t kMaxJttsBytes = 192;
    std::string body;
    if (read_body_str(req, body, kMaxJttsBytes) != ESP_OK) return ESP_OK;
    if (body.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "empty body");
    }
    if (g_jtts_say_sink == nullptr) {
        return send_error(req, "503 Service Unavailable", "jtts sink not registered");
    }
    g_jtts_say_sink(body);
    return send_empty(req);
}

// --- Static root ---

esp_err_t handle_root_get(httpd_req_t* req)
{
    if (!require_auth(req)) {
        ESP_LOGW(kTag, "GET / -> 401 (auth)");
        return ESP_OK;
    }
    const std::size_t len = settings_wifi_html_end - settings_wifi_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    const esp_err_t err = httpd_resp_send(req, settings_wifi_html_start, len);
    ESP_LOGI(kTag, "GET / -> 200, %u B, send=%s",
             static_cast<unsigned>(len), esp_err_to_name(err));
    return err;
}

void add(httpd_handle_t s, const char* uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t*))
{
    httpd_uri_t cfg{};
    cfg.uri = uri;
    cfg.method = m;
    cfg.handler = h;
    cfg.user_ctx = nullptr;
    esp_err_t err = httpd_register_uri_handler(s, &cfg);
    if (err != ESP_OK) {
        // Most likely ESP_ERR_HTTPD_HANDLERS_FULL: max_uri_handlers
        // (wifi_config_service.cpp) is below the number of add() calls below.
        // Make the drop loud so it can't silently 404 a route again.
        ESP_LOGE(kTag, "register_uri_handler(%s) failed: %s — raise cfg.max_uri_handlers",
                 uri, esp_err_to_name(err));
    }
}

} // namespace

void register_handlers(httpd_handle_t server, const config::DeviceConfig& current)
{
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }
    g_active = current;

    // Resolve the live /mcp/* bearer. NVS wins so rotations from the settings
    // UI stick across reboots; the Kconfig token survives as a build-time
    // default for installs that pre-date the NVS field. Empty after both
    // sources means /mcp/* answers 404.
    g_mcp_active_token = current.mcp_api_token;
    if (g_mcp_active_token.empty() && CONFIG_MCP_API_TOKEN[0] != '\0') {
        g_mcp_active_token = CONFIG_MCP_API_TOKEN;
        ESP_LOGI(kTag, "mcp token: NVS empty, using CONFIG_MCP_API_TOKEN fallback");
    } else if (!g_mcp_active_token.empty()) {
        ESP_LOGI(kTag, "mcp token: NVS (%u bytes)",
                 static_cast<unsigned>(g_mcp_active_token.size()));
    } else {
        ESP_LOGI(kTag, "mcp token: none → /mcp/* will answer 404");
    }

    add(server, "/",                    HTTP_GET,  handle_root_get);
    add(server, "/api/status",          HTTP_GET,  handle_status_get);
    add(server, "/api/ssid",            HTTP_POST, handle_ssid_post);
    add(server, "/api/password",        HTTP_POST, handle_password_post);
    add(server, "/api/api-key",         HTTP_POST, handle_api_key_post);
    add(server, "/api/gemini-api-key",  HTTP_POST, handle_gemini_api_key_post);
    add(server, "/api/xiaozhi-url",      HTTP_POST, handle_xiaozhi_url_post);
    add(server, "/api/xiaozhi-token",    HTTP_POST, handle_xiaozhi_token_post);
    add(server, "/api/openai-enabled",  HTTP_POST, handle_openai_enabled_post);
    add(server, "/api/rtp-enabled",     HTTP_POST, handle_rtp_enabled_post);
    add(server, "/api/jtts-idle-enabled", HTTP_POST, handle_jtts_idle_enabled_post);
    add(server, "/api/led-mouth-sync",  HTTP_POST, handle_led_mouth_sync_post);
    add(server, "/api/operation-mode",  HTTP_POST, handle_operation_mode_post);
    add(server, "/api/audio-output",    HTTP_POST, handle_audio_output_post);
    add(server, "/api/lip-sync-mode",   HTTP_POST, handle_lip_sync_mode_post);
    add(server, "/api/mic-lip-agc",     HTTP_POST, handle_mic_lip_agc_post);
    add(server, "/api/barge-in",        HTTP_POST, handle_barge_in_enabled_post);
    add(server, "/api/battery-gauge",   HTTP_POST, handle_battery_gauge_post);
    add(server, "/api/startup-arpeggio",HTTP_POST, handle_startup_arpeggio_post);
    add(server, "/api/servo-enabled",   HTTP_POST, handle_servo_enabled_post);
    add(server, "/api/mcp-token",       HTTP_POST, handle_mcp_token_post);
    add(server, "/api/lt-config",       HTTP_POST, handle_lt_config_post);
    add(server, "/api/provider",        HTTP_POST, handle_provider_post);
    add(server, "/api/jtts-config",     HTTP_POST, handle_jtts_config_post);
    add(server, "/api/servo-limits",    HTTP_POST, handle_servo_limits_post);
    add(server, "/api/servo-range-mode", HTTP_POST, handle_servo_range_mode_post);
    add(server, "/api/system-prompt",   HTTP_POST, handle_system_prompt_post);
    add(server, "/api/conv-headers",     HTTP_POST, handle_conv_headers_post);
    add(server, "/api/settings",        HTTP_GET,  handle_settings_get);
    add(server, "/api/settings",        HTTP_POST, handle_settings_post);
    add(server, "/api/reboot-required", HTTP_GET,  handle_reboot_required_get);
    add(server, "/api/apply",           HTTP_POST, handle_apply_post);
    add(server, "/api/ota/status",      HTTP_GET,  handle_ota_status_get);
    add(server, "/api/ota/control",     HTTP_POST, handle_ota_control_post);
    add(server, "/api/ota/data",        HTTP_POST, handle_ota_data_post);
    add(server, "/api/ota/release",     HTTP_POST, handle_ota_release_post);
    add(server, "/api/camera/capture",  HTTP_GET,  handle_camera_capture_get);
    add(server, "/api/camera/reg",      HTTP_GET,  handle_camera_reg);
    add(server, "/api/camera/reg",      HTTP_POST, handle_camera_reg);
    add(server, "/api/release/versions", HTTP_GET, handle_release_versions_get);
    add(server, "/api/avatar-dsl",       HTTP_POST, handle_avatar_dsl_post);
    add(server, "/api/avatar-dsl/reset", HTTP_POST, handle_avatar_dsl_reset_post);
    add(server, "/api/voice-db",         HTTP_GET,  handle_voice_db_get);
    add(server, "/api/voice-db",         HTTP_POST, handle_voice_db_post);
    add(server, "/api/voice-db/clear",   HTTP_POST, handle_voice_db_clear_post);
    add(server, "/api/hmm-voice",        HTTP_GET,  handle_hmm_voice_get);
    add(server, "/api/hmm-voice",        HTTP_POST, handle_hmm_voice_post);
    add(server, "/api/hmm-voice/clear",  HTTP_POST, handle_hmm_voice_clear_post);
    add(server, "/api/hmm-voice/fetch",  HTTP_POST, handle_hmm_voice_fetch_post);
    add(server, "/api/voices",           HTTP_GET,  handle_voices_get);
    add(server, "/api/metrics/audio",    HTTP_GET,  handle_audio_metrics_get);
    add(server, "/api/led-state",        HTTP_GET,  handle_led_state_get);
    add(server, "/api/led-state",        HTTP_POST, handle_led_state_post);
    add(server, "/api/mic-lip-gain",     HTTP_GET,  handle_mic_lip_gain_get);
    add(server, "/api/mic-lip-gain",     HTTP_POST, handle_mic_lip_gain_post);
    add(server, "/api/speaker-volume",   HTTP_GET,  handle_speaker_volume_get);
    add(server, "/api/speaker-volume",   HTTP_POST, handle_speaker_volume_post);
    add(server, "/api/jtts-say",         HTTP_POST, handle_jtts_say_post);
    add(server, "/api/device-name",     HTTP_POST, handle_device_name_post);
    add(server, "/api/auth-password",   HTTP_POST, handle_auth_password_post);
    // Claude Code Channel adapter API (Bearer-gated). Empty
    // CONFIG_MCP_API_TOKEN keeps these handlers registered but they 404.
    add(server, "/mcp/say",              HTTP_POST, handle_mcp_say_post);
    add(server, "/mcp/expression",       HTTP_POST, handle_mcp_expression_post);
    add(server, "/mcp/balloon",          HTTP_POST, handle_mcp_balloon_post);
    add(server, "/mcp/state",            HTTP_GET,  handle_mcp_state_get);
    add(server, "/mcp/events",           HTTP_GET,  handle_mcp_events_get);
}

void set_wifi_connected(bool connected)
{
    // No mutex: g_wifi_connected is atomic precisely so this can run before
    // g_mutex exists. IP_EVENT_STA_GOT_IP fires ~4 s ahead of the
    // cfg-wifi-init task's register_handlers() (which creates g_mutex), and
    // GOT_IP does not re-fire — the old null-mutex early-return silently
    // dropped the only notification, pinning the flag to false forever.
    g_wifi_connected.store(connected);
}

void set_provisioning_mode(bool active)
{
    // No mutex gate: must stick even before register_handlers() runs (see
    // the g_provisioning_mode declaration comment).
    g_provisioning_mode.store(active);
}

void set_battery(int millivolts, int milliamps, int percent)
{
    if (g_mutex == nullptr) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_battery_mv = millivolts;
    g_battery_ma = milliamps;
    g_battery_pct = percent;
    xSemaphoreGive(g_mutex);
}

void set_servo_range_mode_sink(config::ServoRangeModeSink sink)
{
    if (g_mutex == nullptr) {
        g_servo_range_mode_sink = sink;
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_servo_range_mode_sink = sink;
    xSemaphoreGive(g_mutex);
}

void set_servo_positions_getter(config::ServoPositionsGetter getter)
{
    if (g_mutex == nullptr) {
        g_servo_positions_getter = getter;
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_servo_positions_getter = getter;
    xSemaphoreGive(g_mutex);
}

void set_audio_metrics_getter(config::AudioMetricsJsonGetter getter)
{
    // Plain write; same idempotent registration as the other getters.
    if (g_mutex == nullptr) {
        g_audio_metrics_getter = getter;
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_audio_metrics_getter = getter;
    xSemaphoreGive(g_mutex);
}

void set_led_state_getter(config::LedStateGetter getter)
{
    if (g_mutex == nullptr) { g_led_state_getter = getter; return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_led_state_getter = getter;
    xSemaphoreGive(g_mutex);
}

void set_led_state_sink(config::LedStateSink sink)
{
    if (g_mutex == nullptr) { g_led_state_sink = sink; return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_led_state_sink = sink;
    xSemaphoreGive(g_mutex);
}

void set_mic_lip_gain_getter(config::MicLipGainGetter getter)
{
    if (g_mutex == nullptr) { g_mic_lip_gain_getter = getter; return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_mic_lip_gain_getter = getter;
    xSemaphoreGive(g_mutex);
}

void set_mic_lip_gain_sink(config::MicLipGainSink sink)
{
    if (g_mutex == nullptr) { g_mic_lip_gain_sink = sink; return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_mic_lip_gain_sink = sink;
    xSemaphoreGive(g_mutex);
}

void set_speaker_volume_getter(config::SpeakerVolumeGetter getter)
{
    if (g_mutex == nullptr) { g_speaker_volume_getter = getter; return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_speaker_volume_getter = getter;
    xSemaphoreGive(g_mutex);
}

void set_speaker_volume_sink(config::SpeakerVolumeSink sink)
{
    if (g_mutex == nullptr) { g_speaker_volume_sink = sink; return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_speaker_volume_sink = sink;
    xSemaphoreGive(g_mutex);
}

void set_jtts_say_kana_sink(config::JttsSayKanaSink sink)
{
    if (g_mutex == nullptr) { g_jtts_say_sink = sink; return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_jtts_say_sink = sink;
    xSemaphoreGive(g_mutex);
}

void set_board_kind(std::uint8_t kind)
{
    if (g_mutex == nullptr) {
        g_board_kind = kind;
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_board_kind = kind;
    xSemaphoreGive(g_mutex);
}

void set_avatar_bytecode_sink(AvatarBytecodeSink sink)
{
    if (g_mutex == nullptr) {
        g_avatar_bytecode_sink = std::move(sink);
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_avatar_bytecode_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}

void set_voice_db_sink(VoiceDbSink sink)
{
    if (g_mutex == nullptr) {
        g_voice_db_sink = std::move(sink);
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_voice_db_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}

void set_voice_db_status_getter(VoiceDbStatusGetter getter)
{
    if (g_mutex == nullptr) {
        g_voice_db_status_getter = std::move(getter);
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_voice_db_status_getter = std::move(getter);
    xSemaphoreGive(g_mutex);
}

void set_hmm_voice_sink(HmmVoiceSink sink)
{
    if (g_mutex == nullptr) {
        g_hmm_voice_sink = std::move(sink);
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_hmm_voice_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}

void set_hmm_voice_status_getter(HmmVoiceStatusGetter getter)
{
    if (g_mutex == nullptr) {
        g_hmm_voice_status_getter = std::move(getter);
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_hmm_voice_status_getter = std::move(getter);
    xSemaphoreGive(g_mutex);
}

void set_camera_capture_sink(CameraCaptureSink sink)
{
    if (g_mutex == nullptr) { g_camera_capture_sink = std::move(sink); return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_camera_capture_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}
void set_camera_reg_sink(CameraRegSink sink)
{
    if (g_mutex == nullptr) { g_camera_reg_sink = std::move(sink); return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_camera_reg_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}
void set_mcp_say_kana_sink(McpSayKanaSink sink)
{
    if (g_mutex == nullptr) { g_mcp_say_sink = std::move(sink); return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_mcp_say_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}
void set_lt_config_sink(LtConfigSink sink)
{
    if (g_mutex == nullptr) { g_lt_config_sink = std::move(sink); return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_lt_config_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}
void set_mcp_expression_sink(McpExpressionSink sink)
{
    if (g_mutex == nullptr) { g_mcp_expression_sink = std::move(sink); return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_mcp_expression_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}
void set_mcp_balloon_sink(McpBalloonSink sink)
{
    if (g_mutex == nullptr) { g_mcp_balloon_sink = std::move(sink); return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_mcp_balloon_sink = std::move(sink);
    xSemaphoreGive(g_mutex);
}

} // namespace stackchan::wifi_config::http
