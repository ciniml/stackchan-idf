// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <esp_http_client.h>

namespace stackchan::wifi_config::https {

// Captures the response "Location" header of the most recent request so
// redirects can be followed WITHOUT esp_http_client_set_redirection().
// (IDF ≥ 5.5.5 refuses an https→http Location inside set_redirection with
// ESP_ERR_HTTP_REDIRECT_DOWNGRADE; GitHub Pages' custom-domain 301 is exactly
// that, so we take the Location ourselves and re-issue it over https.)
struct RedirectCapture {
    std::string location;
};

// Wire `cap` into a client config: sets event_handler + user_data. Call
// before esp_http_client_init(). The config must not set its own handler.
void attach(esp_http_client_config_t& cfg, RedirectCapture& cap);

// esp_http_client_open + fetch_headers, following 30x redirects by hand
// (max 3 hops). Every hop is forced onto https://. Returns the final HTTP
// status, or std::nullopt when opening / fetching failed, the hop cap was
// exceeded, or a redirect had no usable Location.
std::optional<int> open_follow_redirects(esp_http_client_handle_t client,
                                         std::int64_t& content_length_out,
                                         RedirectCapture& cap);

}  // namespace stackchan::wifi_config::https
