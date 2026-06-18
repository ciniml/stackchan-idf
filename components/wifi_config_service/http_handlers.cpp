// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "http_handlers.hpp"

#include <avatar_vm/storage.hpp>
#include <config_service/config_store.hpp>
#include <config_service/ota.hpp>
#include <wifi_config_service/mcp_events.hpp>

#include <algorithm>
#include <array>
#include <cJSON.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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

struct StagingBuffer {
    std::optional<std::string> ssid, password, api_key, jtts_config, gemini_api_key, servo_limits;
    std::optional<std::string> xiaozhi_url, xiaozhi_token, system_prompt, conv_headers;
    std::optional<bool> openai_enabled;
    std::optional<bool> rtp_audio_enabled;
    std::optional<bool> jtts_idle_enabled;
    std::optional<bool> battery_gauge_enabled;
    std::optional<bool> servo_enabled;
    std::optional<bool> led_mouth_sync_enabled;
    std::optional<std::string> mcp_api_token;
    std::optional<std::string> lt_config;
    std::optional<config::Provider> provider;
    std::optional<config::OperationMode> operation_mode;
    std::optional<bool> barge_in_enabled;
};

config::DeviceConfig g_active;
StagingBuffer g_staging;
bool g_wifi_connected = false;
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
AvatarBytecodeSink g_avatar_bytecode_sink = nullptr;
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
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const auto cfg = g_active;
    const bool wifi_ok = g_wifi_connected;
    const int bat_mv = g_battery_mv;
    const int bat_ma = g_battery_ma;
    const int bat_pct = g_battery_pct;
    const bool range_mode = g_servo_range_mode;
    config::ServoPositionsGetter pos_getter = g_servo_positions_getter;
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
    body += "\"servo_enabled\":" + std::string(cfg.servo_enabled ? "true" : "false") + ",";
    body += "\"led_mouth_sync_enabled\":" + std::string(cfg.led_mouth_sync_enabled ? "true" : "false") + ",";
    body += "\"operation_mode\":" + std::to_string(static_cast<int>(cfg.operation_mode)) + ",";
    body += "\"barge_in_enabled\":" + std::string(cfg.barge_in_enabled ? "true" : "false") + ",";
    // Token itself is never returned; the UI only needs to know whether the
    // /mcp/* API is reachable (= has a token).
    body += "\"has_mcp_token\":" + std::string(g_mcp_active_token.empty() ? "false" : "true") + ",";
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
    std::string body;
    if (read_body_str(req, body, kMaxSsid) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.ssid = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_password_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxPassword) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.password = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_api_key_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxApiKey) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.api_key = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_gemini_api_key_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxApiKey) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.gemini_api_key = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_xiaozhi_url_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxApiKey) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.xiaozhi_url = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_xiaozhi_token_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxApiKey) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.xiaozhi_token = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_openai_enabled_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.openai_enabled = enabled;
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_rtp_enabled_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.rtp_audio_enabled = enabled;
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_jtts_idle_enabled_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.jtts_idle_enabled = enabled;
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_led_mouth_sync_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.led_mouth_sync_enabled = enabled;
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_operation_mode_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    if (body.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "expected integer 0..2");
    }
    const int v = std::atoi(body.c_str());
    if (v < 0 || v > static_cast<int>(config::OperationMode::Conversation)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "operation_mode out of range");
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.operation_mode = static_cast<config::OperationMode>(v);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_barge_in_enabled_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.barge_in_enabled = enabled;
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_battery_gauge_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.battery_gauge_enabled = enabled;
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_servo_enabled_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const bool enabled = !body.empty() && (body[0] == '1' || body[0] == 't' || body[0] == 'y');
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.servo_enabled = enabled;
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_lt_config_post(httpd_req_t* req)
{
    // LT timekeeper config JSON. Applied live through the sink (demo_loop
    // re-parses off this task's stack) and staged so /api/apply persists it.
    std::string body;
    if (read_body_str(req, body, kMaxJttsConfig) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.lt_config = body;
    LtConfigSink sink = g_lt_config_sink;
    xSemaphoreGive(g_mutex);
    if (sink) sink(body);
    return send_empty(req);
}

esp_err_t handle_mcp_token_post(httpd_req_t* req)
{
    // The token can be anything up to 128 bytes; the firmware just stores it
    // verbatim and constant-time compares against the Authorization header.
    // An empty body explicitly clears the NVS token (next reboot → 404 on
    // /mcp/*, or falls back to the Kconfig build-time default if present).
    std::string body;
    if (read_body_str(req, body, 128) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.mcp_api_token = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_provider_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, 8) != ESP_OK) return ESP_OK;
    const int v = body.empty() ? 0 : (body[0] - '0');
    config::Provider p = config::Provider::OpenAi;
    if (v == static_cast<int>(config::Provider::Gemini)) p = config::Provider::Gemini;
    else if (v == static_cast<int>(config::Provider::XiaoZhi)) p = config::Provider::XiaoZhi;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.provider = p;
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_jtts_config_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxJttsConfig) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.jtts_config = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_servo_limits_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxJttsConfig) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.servo_limits = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_system_prompt_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxSystemPrompt) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.system_prompt = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_conv_headers_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxConvHeaders) != ESP_OK) return ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_staging.conv_headers = std::move(body);
    xSemaphoreGive(g_mutex);
    return send_empty(req);
}

esp_err_t handle_servo_range_mode_post(httpd_req_t* req)
{
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

esp_err_t handle_apply_post(httpd_req_t* req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    config::DeviceConfig merged = g_active;
    if (g_staging.ssid)            merged.wifi_ssid = *g_staging.ssid;
    if (g_staging.password)        merged.wifi_password = *g_staging.password;
    if (g_staging.api_key)         merged.openai_api_key = *g_staging.api_key;
    if (g_staging.openai_enabled)  merged.openai_enabled = *g_staging.openai_enabled;
    if (g_staging.rtp_audio_enabled) merged.rtp_audio_enabled = *g_staging.rtp_audio_enabled;
    if (g_staging.jtts_idle_enabled) merged.jtts_idle_enabled = *g_staging.jtts_idle_enabled;
    if (g_staging.led_mouth_sync_enabled) merged.led_mouth_sync_enabled = *g_staging.led_mouth_sync_enabled;
    if (g_staging.operation_mode) merged.operation_mode = *g_staging.operation_mode;
    if (g_staging.barge_in_enabled) merged.barge_in_enabled = *g_staging.barge_in_enabled;
    if (g_staging.battery_gauge_enabled) merged.battery_gauge_enabled = *g_staging.battery_gauge_enabled;
    if (g_staging.servo_enabled)   merged.servo_enabled = *g_staging.servo_enabled;
    if (g_staging.mcp_api_token)   merged.mcp_api_token = *g_staging.mcp_api_token;
    if (g_staging.lt_config)       merged.lt_config_json = *g_staging.lt_config;
    if (g_staging.jtts_config)     merged.jtts_config_json = *g_staging.jtts_config;
    if (g_staging.servo_limits)    merged.servo_limits_json = *g_staging.servo_limits;
    if (g_staging.gemini_api_key)  merged.gemini_api_key = *g_staging.gemini_api_key;
    if (g_staging.xiaozhi_url)     merged.xiaozhi_url = *g_staging.xiaozhi_url;
    if (g_staging.xiaozhi_token)   merged.xiaozhi_token = *g_staging.xiaozhi_token;
    if (g_staging.system_prompt)   merged.system_prompt = *g_staging.system_prompt;
    if (g_staging.conv_headers)    merged.conv_extra_headers = *g_staging.conv_headers;
    if (g_staging.provider)        merged.provider = *g_staging.provider;
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
    return send_json(req, config::ota::status_json());
}

esp_err_t handle_ota_control_post(httpd_req_t* req)
{
    std::string body;
    if (read_body_str(req, body, kMaxJttsConfig) != ESP_OK) return ESP_OK;
    return send_json(req, config::ota::handle_control_command(body));
}

esp_err_t handle_ota_data_post(httpd_req_t* req)
{
    std::vector<std::uint8_t> body;
    if (read_body(req, body, kMaxOtaChunk) != ESP_OK) return ESP_OK;
    return send_json(req, config::ota::handle_data_chunk({body.data(), body.size()}));
}

// POST /api/avatar-dsl — body is a single complete `.avbc` (no chunking; HTTP
// can carry the whole bytecode in one request). Validates, persists to NVS,
// and applies live via the registered sink. Returns a JSON status. NVS write
// happens before sink invocation so a sink failure leaves the new face active
// after the next reboot anyway.
esp_err_t handle_avatar_dsl_post(httpd_req_t* req)
{
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
    if (g_mcp_active_token.empty()) return false;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    constexpr const char* prefix = "Bearer ";
    constexpr std::size_t plen = 7;
    if (std::strncmp(hdr, prefix, plen) != 0) return false;
    // Constant-time compare on the token tail.
    const char* tok = hdr + plen;
    const std::size_t expected_len = g_mcp_active_token.size();
    if (std::strlen(tok) != expected_len) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < expected_len; ++i) {
        diff |= static_cast<unsigned char>(tok[i] ^ g_mcp_active_token[i]);
    }
    return diff == 0;
}

esp_err_t mcp_gate(httpd_req_t* req)
{
    // 404 (not 401) when the API is entirely disabled so the URL surface
    // is indistinguishable from an unrelated path.
    if (g_mcp_active_token.empty()) {
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
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const bool wifi_ok = g_wifi_connected;
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

// --- Static root ---

esp_err_t handle_root_get(httpd_req_t* req)
{
    const std::size_t len = settings_wifi_html_end - settings_wifi_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, settings_wifi_html_start, len);
}

void add(httpd_handle_t s, const char* uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t*))
{
    httpd_uri_t cfg{};
    cfg.uri = uri;
    cfg.method = m;
    cfg.handler = h;
    cfg.user_ctx = nullptr;
    httpd_register_uri_handler(s, &cfg);
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
    add(server, "/api/barge-in",        HTTP_POST, handle_barge_in_enabled_post);
    add(server, "/api/battery-gauge",   HTTP_POST, handle_battery_gauge_post);
    add(server, "/api/servo-enabled",   HTTP_POST, handle_servo_enabled_post);
    add(server, "/api/mcp-token",       HTTP_POST, handle_mcp_token_post);
    add(server, "/api/lt-config",       HTTP_POST, handle_lt_config_post);
    add(server, "/api/provider",        HTTP_POST, handle_provider_post);
    add(server, "/api/jtts-config",     HTTP_POST, handle_jtts_config_post);
    add(server, "/api/servo-limits",    HTTP_POST, handle_servo_limits_post);
    add(server, "/api/servo-range-mode", HTTP_POST, handle_servo_range_mode_post);
    add(server, "/api/system-prompt",   HTTP_POST, handle_system_prompt_post);
    add(server, "/api/conv-headers",     HTTP_POST, handle_conv_headers_post);
    add(server, "/api/apply",           HTTP_POST, handle_apply_post);
    add(server, "/api/ota/status",      HTTP_GET,  handle_ota_status_get);
    add(server, "/api/ota/control",     HTTP_POST, handle_ota_control_post);
    add(server, "/api/ota/data",        HTTP_POST, handle_ota_data_post);
    add(server, "/api/avatar-dsl",       HTTP_POST, handle_avatar_dsl_post);
    add(server, "/api/avatar-dsl/reset", HTTP_POST, handle_avatar_dsl_reset_post);
    add(server, "/api/metrics/audio",    HTTP_GET,  handle_audio_metrics_get);
    add(server, "/api/led-state",        HTTP_GET,  handle_led_state_get);
    add(server, "/api/led-state",        HTTP_POST, handle_led_state_post);
    add(server, "/api/mic-lip-gain",     HTTP_GET,  handle_mic_lip_gain_get);
    add(server, "/api/mic-lip-gain",     HTTP_POST, handle_mic_lip_gain_post);
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
    if (g_mutex == nullptr) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_wifi_connected = connected;
    xSemaphoreGive(g_mutex);
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
