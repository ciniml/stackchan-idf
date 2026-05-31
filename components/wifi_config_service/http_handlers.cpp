// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "http_handlers.hpp"

#include <config_service/config_store.hpp>
#include <config_service/ota.hpp>

#include <algorithm>
#include <array>
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
    std::optional<bool> battery_gauge_enabled;
    std::optional<config::Provider> provider;
};

config::DeviceConfig g_active;
StagingBuffer g_staging;
bool g_wifi_connected = false;
// Cached battery snapshot served by /api/status. -1 mV / percent → unknown.
int g_battery_mv = -1;
int g_battery_ma = 0;
int g_battery_pct = -1;
esp_timer_handle_t g_restart_timer = nullptr;

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
    xSemaphoreGive(g_mutex);

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
    body += "\"battery_gauge_enabled\":" + std::string(cfg.battery_gauge_enabled ? "true" : "false") + ",";
    body += "\"provider\":" + std::to_string(static_cast<int>(cfg.provider)) + ",";
    body += "\"jtts_config\":\"" + escape_json(cfg.jtts_config_json) + "\",";
    body += "\"servo_limits\":\"" + escape_json(cfg.servo_limits_json) + "\",";
    body += "\"system_prompt\":\"" + escape_json(cfg.system_prompt) + "\",";
    body += "\"conv_extra_headers\":\"" + escape_json(cfg.conv_extra_headers) + "\",";
    body += "\"battery_mv\":" + std::to_string(bat_mv) + ",";
    body += "\"battery_ma\":" + std::to_string(bat_ma) + ",";
    body += "\"battery_pct\":" + std::to_string(bat_pct);
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

esp_err_t handle_apply_post(httpd_req_t* req)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    config::DeviceConfig merged = g_active;
    if (g_staging.ssid)            merged.wifi_ssid = *g_staging.ssid;
    if (g_staging.password)        merged.wifi_password = *g_staging.password;
    if (g_staging.api_key)         merged.openai_api_key = *g_staging.api_key;
    if (g_staging.openai_enabled)  merged.openai_enabled = *g_staging.openai_enabled;
    if (g_staging.rtp_audio_enabled) merged.rtp_audio_enabled = *g_staging.rtp_audio_enabled;
    if (g_staging.battery_gauge_enabled) merged.battery_gauge_enabled = *g_staging.battery_gauge_enabled;
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
    add(server, "/api/battery-gauge",   HTTP_POST, handle_battery_gauge_post);
    add(server, "/api/provider",        HTTP_POST, handle_provider_post);
    add(server, "/api/jtts-config",     HTTP_POST, handle_jtts_config_post);
    add(server, "/api/servo-limits",    HTTP_POST, handle_servo_limits_post);
    add(server, "/api/system-prompt",   HTTP_POST, handle_system_prompt_post);
    add(server, "/api/conv-headers",     HTTP_POST, handle_conv_headers_post);
    add(server, "/api/apply",           HTTP_POST, handle_apply_post);
    add(server, "/api/ota/status",      HTTP_GET,  handle_ota_status_get);
    add(server, "/api/ota/control",     HTTP_POST, handle_ota_control_post);
    add(server, "/api/ota/data",        HTTP_POST, handle_ota_data_post);
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

} // namespace stackchan::wifi_config::http
