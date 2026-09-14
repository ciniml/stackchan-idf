// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "http.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <mbedtls/base64.h>

#include <config_service/ota.hpp>

#include "release_ota.hpp"
#include "wifi.hpp"

namespace stackchan::recovery::http {

namespace {

constexpr const char* kTag = "rcv-http";
constexpr std::size_t kMaxControlBytes = 960;
constexpr std::size_t kMaxOtaChunk = 4096;

std::string g_auth_password;
std::uint8_t g_board_kind = 0;
httpd_handle_t g_server = nullptr;

bool constant_time_equals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned>(a[i]) ^ static_cast<unsigned>(b[i]);
    }
    return diff == 0;
}

esp_err_t send_json(httpd_req_t* req, const std::string& body)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t send_error(httpd_req_t* req, const char* status, const char* msg)
{
    httpd_resp_set_status(req, status);
    char buf[128];
    std::snprintf(buf, sizeof(buf), R"({"ok":false,"error":"%s"})", msg);
    return send_json(req, buf);
}

// Main (wifi_config_service/http_handlers.cpp) と同じ Basic 認証。
bool require_auth(httpd_req_t* req)
{
    if (g_auth_password.empty()) return true;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        std::strncmp(hdr, "Basic ", 6) == 0) {
        const char* b64 = hdr + 6;
        const std::size_t b64_len = std::strlen(b64);
        std::vector<unsigned char> dec(b64_len + 3, 0);
        std::size_t olen = 0;
        if (mbedtls_base64_decode(dec.data(), dec.size(), &olen,
                                  reinterpret_cast<const unsigned char*>(b64), b64_len) == 0) {
            std::string up(reinterpret_cast<const char*>(dec.data()), olen);
            const auto colon = up.find(':');
            if (colon != std::string::npos && constant_time_equals(up.substr(colon + 1), g_auth_password)) {
                return true;
            }
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"stackchan\"");
    httpd_resp_send(req, nullptr, 0);
    return false;
}

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
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(out.data() + off), out.size() - off);
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

esp_err_t handle_status_get(httpd_req_t* req)
{
    const esp_app_desc_t* desc = esp_app_get_description();
    char buf[160];
    std::snprintf(buf, sizeof(buf), R"({"mode":"recovery","version":"%s","wifi":%s})",
                  desc != nullptr ? desc->version : "?", wifi::connected() ? "true" : "false");
    return send_json(req, buf);
}

esp_err_t handle_ota_status_get(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    return send_json(req, config::ota::status_json());
}

esp_err_t handle_ota_control_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    std::string body;
    if (read_body_str(req, body, kMaxControlBytes) != ESP_OK) return ESP_OK;
    if (body.find("\"abort\"") != std::string::npos) {
        wifi_config::release_ota::request_abort();
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

esp_err_t handle_ota_release_post(httpd_req_t* req)
{
    if (!require_auth(req)) return ESP_OK;
    if (!wifi::connected()) {
        return send_error(req, "409 Conflict", "sta not connected");
    }
    std::string body;
    if (read_body_str(req, body, 256) != ESP_OK) return ESP_OK;

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return send_error(req, "400 Bad Request", "bad json");
    const cJSON* tag = cJSON_GetObjectItemCaseSensitive(root, "tag");
    if (!cJSON_IsString(tag) || tag->valuestring == nullptr) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "tag required");
    }
    const std::string tag_str = tag->valuestring;
    cJSON_Delete(root);

    using wifi_config::release_ota::StartError;
    auto r = wifi_config::release_ota::start(tag_str, g_board_kind);
    if (!r) {
        const char* msg = "?";
        switch (r.error()) {
        case StartError::AlreadyRunning:    msg = "already running"; break;
        case StartError::BadTag:            msg = "bad tag";         break;
        case StartError::UnknownBoard:      msg = "unknown board";   break;
        case StartError::WorkerSpawnFailed: msg = "spawn failed";    break;
        }
        return send_error(req, "400 Bad Request", msg);
    }
    return send_json(req, R"({"ok":true,"queued":true})");
}

void add(httpd_handle_t server, const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*))
{
    httpd_uri_t u{};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    if (httpd_register_uri_handler(server, &u) != ESP_OK) {
        ESP_LOGE(kTag, "register %s failed", uri);
    }
}

} // namespace

void start(const std::string& auth_password, std::uint8_t board_kind)
{
    g_auth_password = auth_password;
    g_board_kind = board_kind;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 8;
    // OTA データ POST は 4 KiB のボディを受けて esp_ota_write する。
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&g_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start: %s", esp_err_to_name(err));
        return;
    }
    add(g_server, "/api/status",      HTTP_GET,  handle_status_get);
    add(g_server, "/api/ota/status",  HTTP_GET,  handle_ota_status_get);
    add(g_server, "/api/ota/control", HTTP_POST, handle_ota_control_post);
    add(g_server, "/api/ota/data",    HTTP_POST, handle_ota_data_post);
    add(g_server, "/api/ota/release", HTTP_POST, handle_ota_release_post);
    ESP_LOGI(kTag, "listening on :80 (auth %s)", auth_password.empty() ? "off" : "on");
}

} // namespace stackchan::recovery::http
