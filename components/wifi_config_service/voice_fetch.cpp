// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "voice_fetch.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "https_fetch.hpp"

namespace stackchan::wifi_config::voice_fetch {

namespace {

constexpr const char* kTag = "voice-fetch";

// ダウンロード上限 (voice パーティションのデータ容量 = 4 MiB − ヘッダ)。
constexpr std::size_t kMaxBytes = 4 * 1024 * 1024 - 16;
constexpr std::size_t kReadChunk = 8192;
constexpr int kManifestMaxBytes = 16 * 1024;

bool id_looks_safe(const std::string& id) {
    // ボイス ID はファイル名の一部 (voices/<id>.htsvoice) になるので、URL/パス
    // に安全な文字だけ許す。".." も弾いてトラバーサルを封じる。
    if (id.empty() || id.size() > 32) return false;
    if (id.find("..") != std::string::npos) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

const char* fetch_and_install(const std::string& voice_id, const InstallFn& install) {
    if (!id_looks_safe(voice_id)) return "bad voice id";

    char url[256];
    std::snprintf(url, sizeof(url),
                  "https://ciniml.github.io/stackchan-idf/voices/%s.htsvoice",
                  voice_id.c_str());
    ESP_LOGI(kTag, "GET %s", url);

    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 30000;
    cfg.keep_alive_enable = false;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 4;

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
        return "voice not found on server (HTTP != 200)";
    }
    if (cl <= 0 || static_cast<std::size_t>(cl) > kMaxBytes) {
        cleanup();
        return "bad content length";
    }

    auto* buf = static_cast<std::uint8_t*>(
        heap_caps_malloc(static_cast<std::size_t>(cl), MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        cleanup();
        return "no PSRAM for download buffer";
    }
    std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)> guard(buf, &heap_caps_free);

    std::size_t total = 0;
    while (total < static_cast<std::size_t>(cl)) {
        const int n = esp_http_client_read(
            client, reinterpret_cast<char*>(buf + total),
            std::min(kReadChunk, static_cast<std::size_t>(cl) - total));
        if (n <= 0) {
            cleanup();
            return "download read error / early EOF";
        }
        total += static_cast<std::size_t>(n);
    }
    cleanup();

    // インストール (flash 書き込み + パース)。install は hmm_voice::store。
    const char* err = install(buf, static_cast<std::size_t>(cl));
    if (err != nullptr) return err;
    ESP_LOGI(kTag, "voice '%s' installed (%u bytes)", voice_id.c_str(),
             static_cast<unsigned>(cl));
    return nullptr;
}

bool fetch_manifest(std::string& out) {
    esp_http_client_config_t cfg{};
    cfg.url = "https://ciniml.github.io/stackchan-idf/voices.json";
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.keep_alive_enable = false;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 4;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;

    bool ok = false;
    do {
        std::int64_t cl = 0;
        const std::optional<int> status_opt = https::open_follow_redirects(client, cl);
        if (!status_opt.has_value() || *status_opt != 200) break;
        if (cl <= 0 || cl > kManifestMaxBytes) break;
        out.resize(static_cast<std::size_t>(cl));
        int total = 0;
        while (total < cl) {
            const int n = esp_http_client_read(client, out.data() + total, cl - total);
            if (n <= 0) {
                total = -1;
                break;
            }
            total += n;
        }
        if (total == cl) ok = true;
    } while (false);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

}  // namespace stackchan::wifi_config::voice_fetch
