// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "https_fetch.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#include <esp_log.h>

namespace stackchan::wifi_config::https {

namespace {
constexpr const char* kTag = "https-fetch";
}  // namespace

// Streaming mode (esp_http_client_open + fetch_headers + read) does NOT honour
// the client's auto-redirect: a 30x is handed back to us verbatim. GitHub Pages
// with a custom domain (www.fugafuga.org) answers the ciniml.github.io path with
// a 301, so we have to walk the Location chain by hand.
//
// After a 30x, esp_http_client_set_redirection() copies the server's Location
// into the client URL (reparsing the scheme). We then re-open + re-fetch. We cap
// the hops and UPGRADE any http:// Location to https:// before reconnecting —
// GitHub Pages' custom-domain (www.fugafuga.org) 301 hands back a literal
// plaintext "http://..." URL, but the 301 itself arrived over GitHub's valid
// TLS so the Location can't have been tampered with. Reconnecting to the same
// host/path under https means plaintext never touches the wire (an HSTS-style
// upgrade); if that host doesn't actually speak https the TLS handshake simply
// fails. get_transport_type() is unreliable right after close() (it returns a
// stale value), so the https guarantee is enforced by rewriting the URL string.
//
// Returns the final (non-redirect) HTTP status, or std::nullopt if opening /
// fetching failed, the hop cap was exceeded, or a redirect pointed at a URL
// with neither an http:// nor https:// scheme.
constexpr int kMaxRedirects = 3;

std::optional<int> open_follow_redirects(esp_http_client_handle_t client,
                                         std::int64_t& content_length_out)
{
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
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
            ESP_LOGE(kTag, "too many redirects (>%d), last status %d",
                     kMaxRedirects, status);
            return std::nullopt;
        }

        // Point the client at Location. Must close the current connection
        // first — set_redirection only rewrites the URL, the next open()
        // establishes the new connection.
        esp_http_client_close(client);
        if (esp_err_t e = esp_http_client_set_redirection(client); e != ESP_OK) {
            ESP_LOGE(kTag, "set_redirection (status %d): %s", status,
                     esp_err_to_name(e));
            return std::nullopt;
        }
        // Force the redirect target onto https before we reconnect: the
        // Location arrived over trusted TLS, so upgrading http:// -> https://
        // keeps the whole chain encrypted (plaintext never touches the wire).
        //
        // Subtlety confirmed against IDF 5.5.4 esp_http_client.c: after
        // set_redirection() parses a plaintext "http://host/path" Location,
        // connection_info.port is pinned to 80, and esp_http_client_get_url()
        // ALWAYS re-emits the port explicitly ("%s://%s:%d%s") — i.e. we get
        // back "http://host:80/path". A naive "http://"->"https://" splice
        // would leave the stale ":80", and esp_http_client_set_url()'s UF_PORT
        // branch (which runs AFTER the scheme->default-port branch) would then
        // override the freshly-defaulted 443 back to 80, so the TLS handshake
        // lands on port 80 and fails with ESP_ERR_HTTP_CONNECT. Fix: rebuild
        // the URL with an https-appropriate port. Drop the port when it's the
        // http default (80) so https defaults to 443; keep any genuinely
        // explicit non-default port (a Location that named its own port).
        char url[256];
        if (esp_err_t e = esp_http_client_get_url(client, url, sizeof(url)); e != ESP_OK) {
            ESP_LOGE(kTag, "get_url after redirect (status %d): %s", status,
                     esp_err_to_name(e));
            return std::nullopt;
        }
        if (std::strncmp(url, "http://", 7) == 0) {
            // Split "http://<hostport>/<rest...>" at the first '/' after the
            // scheme. <hostport> is "host" or "host:port"; <rest> (with its
            // leading '/') is the path+query, or empty.
            const char* host_start = url + 7;
            const char* path_start = std::strchr(host_start, '/');
            const std::size_t host_len =
                path_start ? static_cast<std::size_t>(path_start - host_start)
                           : std::strlen(host_start);
            const char* rest = path_start ? path_start : "";

            // Separate host from an optional explicit port.
            std::string_view hostport(host_start, host_len);
            const std::size_t colon = hostport.rfind(':');
            std::string_view host = hostport;
            std::string_view port; // empty => no explicit port
            if (colon != std::string_view::npos) {
                host = hostport.substr(0, colon);
                port = hostport.substr(colon + 1);
            }

            // 80 is the http default that get_url synthesised — drop it so
            // https resolves to 443. Anything else was a real explicit port
            // and must be preserved.
            const bool port_is_http_default = (port == "80");

            char upgraded[256 + 8];
            int written;
            if (port.empty() || port_is_http_default) {
                // No explicit port (or the synthetic http default): pin :443
                // so set_url's UF_PORT branch can't fall back to an http port.
                written = std::snprintf(upgraded, sizeof(upgraded),
                                        "https://%.*s:443%s",
                                        static_cast<int>(host.size()), host.data(), rest);
            } else {
                // Honour the Location's explicit non-default port under https.
                written = std::snprintf(upgraded, sizeof(upgraded),
                                        "https://%.*s:%.*s%s",
                                        static_cast<int>(host.size()), host.data(),
                                        static_cast<int>(port.size()), port.data(), rest);
            }
            if (written < 0 || static_cast<std::size_t>(written) >= sizeof(upgraded)) {
                ESP_LOGE(kTag, "redirect URL too long to upgrade (status %d)", status);
                return std::nullopt;
            }
            ESP_LOGI(kTag, "upgrading redirect to https (status %d): %s", status, upgraded);
            if (esp_err_t e = esp_http_client_set_url(client, upgraded); e != ESP_OK) {
                ESP_LOGE(kTag, "set_url after upgrade (status %d): %s", status,
                         esp_err_to_name(e));
                return std::nullopt;
            }
        } else if (std::strncmp(url, "https://", 8) != 0) {
            ESP_LOGE(kTag, "refusing redirect with non-http(s) scheme (status %d)", status);
            return std::nullopt;
        }
        ESP_LOGI(kTag, "following redirect %d (status %d)", hop + 1, status);
    }
    return std::nullopt;
}


}  // namespace stackchan::wifi_config::https
