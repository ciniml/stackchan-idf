// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "https_fetch.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <esp_log.h>

namespace stackchan::wifi_config::https {

namespace {

constexpr const char* kTag = "https-fetch";
constexpr int kMaxRedirects = 3;
constexpr std::size_t kMaxLocationBytes = 1024;

bool iequals(const char* a, const char* b)
{
    for (;; ++a, ++b) {
        const int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        const int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return false;
        if (ca == 0) return true;
    }
}

esp_err_t event_handler(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || evt->user_data == nullptr) return ESP_OK;
    if (evt->header_key == nullptr || evt->header_value == nullptr) return ESP_OK;
    if (!iequals(evt->header_key, "Location")) return ESP_OK;
    auto& cap = *static_cast<RedirectCapture*>(evt->user_data);
    cap.location.assign(evt->header_value);
    if (cap.location.size() > kMaxLocationBytes) cap.location.clear();
    return ESP_OK;
}

// Build the https:// URL to reconnect to from a raw Location value.
//   https://host[:port]/path  -> unchanged
//   http://host/path          -> https://host/path   (upgrade; the 301 itself
//                                arrived over trusted TLS, so the target
//                                can't have been tampered with and plaintext
//                                never touches the wire)
//   http://host:80/path       -> https://host/path   (drop the http default
//                                port so https resolves to 443)
//   http://host:8080/path     -> https://host:8080/path
//   /relative                 -> not supported (nullopt)
std::optional<std::string> to_https(const std::string& loc)
{
    if (loc.rfind("https://", 0) == 0) return loc;
    if (loc.rfind("http://", 0) != 0) return std::nullopt;
    const std::string_view rest_v{loc.data() + 7, loc.size() - 7};
    const std::size_t slash = rest_v.find('/');
    std::string_view hostport = rest_v.substr(0, slash);
    std::string_view path = slash == std::string_view::npos ? std::string_view{} : rest_v.substr(slash);
    std::string_view host = hostport;
    std::string_view port;
    if (const std::size_t colon = hostport.rfind(':'); colon != std::string_view::npos) {
        host = hostport.substr(0, colon);
        port = hostport.substr(colon + 1);
    }
    std::string out = "https://";
    out.append(host);
    if (!port.empty() && port != "80") {
        out += ':';
        out.append(port);
    }
    if (path.empty()) out += '/';
    else out.append(path);
    return out;
}

}  // namespace

void attach(esp_http_client_config_t& cfg, RedirectCapture& cap)
{
    cfg.event_handler = &event_handler;
    cfg.user_data = &cap;
    // We follow redirects ourselves; the client must hand 30x back to us.
    cfg.disable_auto_redirect = true;
}

// Streaming mode (esp_http_client_open + fetch_headers + read) hands a 30x
// back to us verbatim. GitHub Pages with a custom domain (www.fugafuga.org)
// answers the ciniml.github.io path with a 301 whose Location is a literal
// plaintext http:// URL. We never call esp_http_client_set_redirection():
// IDF ≥ 5.5.5 rejects that Location as a transport downgrade
// (ESP_ERR_HTTP_REDIRECT_DOWNGRADE) and older IDF re-emits the URL with an
// explicit ":80" that then survives the https upgrade and breaks the TLS
// connect. Instead the Location captured by the header event is rewritten
// onto https:// and applied with esp_http_client_set_url().
std::optional<int> open_follow_redirects(esp_http_client_handle_t client,
                                         std::int64_t& content_length_out,
                                         RedirectCapture& cap)
{
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        cap.location.clear();
        if (esp_err_t e = esp_http_client_open(client, 0); e != ESP_OK) {
            ESP_LOGE(kTag, "http_client_open: %s", esp_err_to_name(e));
            return std::nullopt;
        }
        content_length_out = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);

        const bool is_redirect =
            status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308;
        if (!is_redirect) {
            return status;
        }
        if (hop == kMaxRedirects) {
            ESP_LOGE(kTag, "too many redirects (>%d), last status %d", kMaxRedirects, status);
            return std::nullopt;
        }
        if (cap.location.empty()) {
            ESP_LOGE(kTag, "redirect (status %d) without a usable Location header", status);
            return std::nullopt;
        }
        const std::optional<std::string> next = to_https(cap.location);
        if (!next) {
            ESP_LOGE(kTag, "refusing redirect to non-http(s) / relative Location (status %d)", status);
            return std::nullopt;
        }
        // Close the current connection first — set_url only rewrites the
        // target, the next open() establishes the new connection.
        esp_http_client_close(client);
        if (esp_err_t e = esp_http_client_set_url(client, next->c_str()); e != ESP_OK) {
            ESP_LOGE(kTag, "set_url after redirect (status %d): %s", status, esp_err_to_name(e));
            return std::nullopt;
        }
        ESP_LOGI(kTag, "following redirect %d (status %d) -> %s", hop + 1, status, next->c_str());
    }
    return std::nullopt;
}

}  // namespace stackchan::wifi_config::https
