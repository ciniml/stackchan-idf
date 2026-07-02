// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/ota.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_system.h>

namespace stackchan::config::ota {

namespace {

constexpr const char* kTag = "cfg-ota";

// The esp_app_desc_t lives immediately after the image header + first segment
// header, i.e. at this byte offset from the start of the .bin.
constexpr std::size_t kAppDescOffset =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
// We only need to see up to the end of project_name to validate. Everything
// past that (time / date / idf_ver / sha256 …) is irrelevant here, so cap the
// header sniff at project_name's end to keep the staging buffer tiny.
constexpr std::size_t kProjectNameOffset =
    kAppDescOffset + offsetof(esp_app_desc_t, project_name);
constexpr std::size_t kProjectNameSize = sizeof(esp_app_desc_t::project_name);
constexpr std::size_t kHeaderSniffBytes = kProjectNameOffset + kProjectNameSize;

enum class Phase : std::uint8_t { Idle, Receiving, Done, Failed };

struct State {
    Phase phase = Phase::Idle;
    esp_ota_handle_t handle = 0;
    const esp_partition_t* partition = nullptr;
    std::size_t total = 0;
    std::size_t received = 0;
    std::string error;
    // Header sniff: the leading bytes of the image are copied here as they
    // arrive so project_name can be validated once we've seen enough, even if
    // the transport splits the esp_app_desc_t across several small chunks (the
    // BLE path can deliver ~500 B frames). Validation runs exactly once.
    std::array<std::uint8_t, kHeaderSniffBytes> header{};
    std::size_t header_len = 0;
    bool app_desc_checked = false;
};

static State g_state;
static esp_timer_handle_t g_reboot_timer = nullptr;

const char* phase_name(Phase p)
{
    switch (p) {
    case Phase::Idle:      return "idle";
    case Phase::Receiving: return "receiving";
    case Phase::Done:      return "done";
    case Phase::Failed:    return "failed";
    }
    return "?";
}

void mark_failed(const char* msg)
{
    ESP_LOGE(kTag, "OTA failed: %s", msg);
    if (g_state.phase == Phase::Receiving && g_state.handle != 0) {
        esp_ota_abort(g_state.handle);
    }
    g_state.handle = 0;
    g_state.phase = Phase::Failed;
    g_state.error = msg;
}

std::string make_status()
{
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  R"({"state":"%s","received":%u,"total":%u,"error":"%s"})",
                  phase_name(g_state.phase),
                  static_cast<unsigned>(g_state.received),
                  static_cast<unsigned>(g_state.total),
                  g_state.error.c_str());
    return std::string(buf);
}

std::string error_response(const char* msg)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), R"({"ok":false,"error":"%s"})", msg);
    return std::string(buf);
}

// Stage the leading bytes of the incoming image and, once we've buffered
// through the end of esp_app_desc_t::project_name, verify it matches our own
// project name. Rejects images built for a *different* project (e.g. a
// wrong-firmware upload) before esp_ota_set_boot_partition can make them
// bootable. Returns std::nullopt on OK / not-yet-enough-bytes, or an error
// message to reject with.
//
// NOTE: all stackchan boards (CoreS3 / AtomS3 / StopWatch) share the same
// CMake project name "stackchan_idf", so this check does NOT catch a
// cross-board mismatch (e.g. a StopWatch image flashed onto a CoreS3). Its
// job is only to reject a genuinely foreign project's firmware. Per-board
// safety still relies on the release-fetch path picking the right board slug.
std::optional<const char*> sniff_and_check_app_desc(std::span<const std::uint8_t> data)
{
    if (g_state.app_desc_checked) {
        return std::nullopt;
    }
    // Copy as much of the still-missing header prefix as this chunk provides.
    if (g_state.header_len < kHeaderSniffBytes) {
        const std::size_t want = kHeaderSniffBytes - g_state.header_len;
        const std::size_t take = std::min(want, data.size());
        std::memcpy(g_state.header.data() + g_state.header_len, data.data(), take);
        g_state.header_len += take;
    }
    if (g_state.header_len < kHeaderSniffBytes) {
        return std::nullopt; // wait for more bytes
    }

    // Reject an image whose magic byte isn't a valid ESP32 app image up front —
    // esp_ota_write would catch this too, but failing here keeps the error
    // path uniform with the project_name reject below.
    if (g_state.header[0] != ESP_IMAGE_HEADER_MAGIC) {
        g_state.app_desc_checked = true;
        return "not an esp32 image";
    }

    const char* incoming = reinterpret_cast<const char*>(g_state.header.data() + kProjectNameOffset);
    // project_name is a fixed 32-byte field; treat it as NUL-terminated but
    // bound the compare so a non-terminated field can't run off the buffer.
    const esp_app_desc_t* self = esp_app_get_description();
    g_state.app_desc_checked = true;
    if (self == nullptr) {
        return std::nullopt; // can't compare — don't block the update
    }
    if (std::strncmp(incoming, self->project_name, kProjectNameSize) != 0) {
        char name[kProjectNameSize + 1] = {};
        std::memcpy(name, incoming, kProjectNameSize);
        ESP_LOGE(kTag, "project_name mismatch: image='%s' expected='%s'",
                 name, self->project_name);
        return "project name mismatch";
    }
    ESP_LOGI(kTag, "app_desc project_name '%s' matches — accepting image",
             self->project_name);
    return std::nullopt;
}

void reboot_cb(void* /*arg*/)
{
    ESP_LOGI(kTag, "rebooting into new firmware");
    esp_restart();
}

std::string cmd_begin(const cJSON* root)
{
    const cJSON* size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsNumber(size_item) || size_item->valuedouble <= 0) {
        return error_response("size required");
    }
    const std::size_t size = static_cast<std::size_t>(size_item->valuedouble);

    if (g_state.phase == Phase::Receiving && g_state.handle != 0) {
        esp_ota_abort(g_state.handle);
    }
    g_state = State{};

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (part == nullptr) {
        mark_failed("no OTA partition");
        return error_response("no OTA partition");
    }
    if (size > part->size) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "size %u exceeds partition %u",
                      static_cast<unsigned>(size), static_cast<unsigned>(part->size));
        mark_failed(msg);
        return error_response(msg);
    }
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, size, &h);
    if (err != ESP_OK) {
        mark_failed(esp_err_to_name(err));
        return error_response(esp_err_to_name(err));
    }
    g_state.handle = h;
    g_state.partition = part;
    g_state.total = size;
    g_state.received = 0;
    g_state.phase = Phase::Receiving;
    g_state.error.clear();
    ESP_LOGI(kTag, "OTA begin: partition '%s' (addr=0x%lx size=%u), image=%u bytes",
             part->label, (unsigned long)part->address,
             static_cast<unsigned>(part->size), static_cast<unsigned>(size));
    return R"({"ok":true})";
}

std::string cmd_end()
{
    if (g_state.phase != Phase::Receiving) {
        return error_response("not receiving");
    }
    if (g_state.received != g_state.total) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "incomplete (%u/%u)",
                      static_cast<unsigned>(g_state.received),
                      static_cast<unsigned>(g_state.total));
        mark_failed(msg);
        return error_response(msg);
    }
    esp_err_t err = esp_ota_end(g_state.handle);
    g_state.handle = 0;
    if (err != ESP_OK) {
        mark_failed(esp_err_to_name(err));
        return error_response(esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(g_state.partition);
    if (err != ESP_OK) {
        mark_failed(esp_err_to_name(err));
        return error_response(esp_err_to_name(err));
    }
    g_state.phase = Phase::Done;
    ESP_LOGI(kTag, "OTA done, scheduling reboot in 500 ms");

    if (g_reboot_timer == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = reboot_cb;
        args.name = "ota_reboot";
        esp_timer_create(&args, &g_reboot_timer);
    }
    esp_timer_start_once(g_reboot_timer, 500'000); // 500 ms — let the ATT response flush

    return R"({"ok":true,"reboot":true})";
}

std::string cmd_abort()
{
    abort_update();
    return R"({"ok":true})";
}

} // namespace

std::string handle_control_command(const std::string& json)
{
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return error_response("bad json");
    }
    const cJSON* op = cJSON_GetObjectItemCaseSensitive(root, "op");
    std::string result;
    if (cJSON_IsString(op) && op->valuestring != nullptr) {
        if (std::strcmp(op->valuestring, "begin") == 0) {
            result = cmd_begin(root);
        } else if (std::strcmp(op->valuestring, "end") == 0) {
            result = cmd_end();
        } else if (std::strcmp(op->valuestring, "abort") == 0) {
            result = cmd_abort();
        } else {
            result = error_response("unknown op");
        }
    } else {
        result = error_response("op required");
    }
    cJSON_Delete(root);
    return result;
}

std::string handle_data_chunk(std::span<const std::uint8_t> data)
{
    if (g_state.phase != Phase::Receiving || g_state.handle == 0) {
        return error_response("not receiving");
    }
    if (data.empty()) {
        return error_response("empty chunk");
    }
    if (g_state.received + data.size() > g_state.total) {
        mark_failed("overrun");
        return error_response("overrun");
    }
    // Validate the embedded esp_app_desc_t::project_name against our own before
    // committing bytes to flash. Runs across the BLE / HTTP-upload / release
    // fetch paths since all three funnel through here. On mismatch we abort so
    // esp_ota_end / esp_ota_set_boot_partition are never reached.
    if (std::optional<const char*> reject = sniff_and_check_app_desc(data); reject) {
        mark_failed(*reject);
        return error_response(*reject);
    }
    esp_err_t err = esp_ota_write(g_state.handle, data.data(), data.size());
    if (err != ESP_OK) {
        mark_failed(esp_err_to_name(err));
        return error_response(esp_err_to_name(err));
    }
    g_state.received += data.size();
    return make_status();
}

std::string status_json()
{
    return make_status();
}

void abort_update()
{
    if (g_state.phase == Phase::Receiving && g_state.handle != 0) {
        esp_ota_abort(g_state.handle);
        ESP_LOGW(kTag, "OTA aborted at %u/%u bytes",
                 static_cast<unsigned>(g_state.received),
                 static_cast<unsigned>(g_state.total));
    }
    g_state = State{};
}

} // namespace stackchan::config::ota
