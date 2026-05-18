// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include <config_service/ota.hpp>

#include <cstdio>
#include <cstring>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_system.h>

namespace stackchan::config::ota {

namespace {

constexpr const char* kTag = "cfg-ota";

enum class Phase : std::uint8_t { Idle, Receiving, Done, Failed };

struct State {
    Phase phase = Phase::Idle;
    esp_ota_handle_t handle = 0;
    const esp_partition_t* partition = nullptr;
    std::size_t total = 0;
    std::size_t received = 0;
    std::string error;
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
    abort();
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

void abort()
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
