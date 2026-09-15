// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "sano_fetch.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "https_fetch.hpp"

namespace stackchan::wifi_config::sano_fetch {

namespace {

constexpr const char* kTag = "sano-fetch";
constexpr std::size_t kMaxBytes = 1024 * 1024;  // 現行 blob 654,032 B。領域は 704 KiB
constexpr std::size_t kReadChunk = 4096;

bool name_looks_safe(const std::string& s, std::size_t max_len) {
    if (s.empty() || s.size() > max_len || s.find("..") != std::string::npos) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

const char* fetch_and_install(const std::string& release_tag, const std::string& file_name,
                              const InstallFn& install) {
    if (!name_looks_safe(release_tag, 32) || !name_looks_safe(file_name, 64)) return "bad release/file name";

    char url[256];
    std::snprintf(url, sizeof(url), "https://github.com/ayutaz/sanoTTS-jp/releases/download/%s/%s",
                  release_tag.c_str(), file_name.c_str());
    ESP_LOGI(kTag, "GET %s", url);

    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 30000;
    cfg.keep_alive_enable = false;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 4;
    // GitHub の Releases は objects.githubusercontent.com の署名付き URL (数百
    // バイトの Location ヘッダ) へリダイレクトする。既定 512 B の受信バッファでは
    // "Out of buffer" で open に失敗する。
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return "http client init failed";
    auto cleanup = [&]() {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    };

    std::int64_t cl = 0;
    const std::optional<int> status_opt = https::open_follow_redirects(client, cl);
    if (!status_opt.has_value()) {
        cleanup();
        return "connect / redirect failed (STA down?)";
    }
    if (*status_opt != 200) {
        cleanup();
        return "asset not found on GitHub (HTTP != 200)";
    }
    if (cl <= 0 || static_cast<std::size_t>(cl) > kMaxBytes) {
        cleanup();
        return "bad content length";
    }

    auto* buf = static_cast<std::uint8_t*>(heap_caps_malloc(static_cast<std::size_t>(cl), MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        cleanup();
        return "no PSRAM for download buffer";
    }
    std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)> guard(buf, &heap_caps_free);

    std::size_t total = 0;
    while (total < static_cast<std::size_t>(cl)) {
        const int n = esp_http_client_read(client, reinterpret_cast<char*>(buf + total),
                                           std::min(kReadChunk, static_cast<std::size_t>(cl) - total));
        if (n <= 0) {
            cleanup();
            return "download read error / early EOF";
        }
        total += static_cast<std::size_t>(n);
    }
    cleanup();

    const char* err = install(buf, static_cast<std::size_t>(cl));
    if (err != nullptr) return err;
    ESP_LOGI(kTag, "%s/%s installed (%u bytes)", release_tag.c_str(), file_name.c_str(),
             static_cast<unsigned>(cl));
    return nullptr;
}

}  // namespace stackchan::wifi_config::sano_fetch
