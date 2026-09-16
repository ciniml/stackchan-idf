// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "crash_report/crash_report.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>

#include <sdkconfig.h>
#include <esp_core_dump.h>
#include <esp_log.h>
#include <esp_system.h>

namespace stackchan::crash_report {

namespace {

constexpr const char* kTag = "crash";

const char* reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                 return "UNKNOWN";
    }
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
bool read_summary(esp_core_dump_summary_t& s)
{
    if (esp_core_dump_image_check() != ESP_OK) return false;
    std::memset(&s, 0, sizeof(s));
    return esp_core_dump_get_summary(&s) == ESP_OK;
}
#endif

} // namespace

void init()
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_core_dump_init();
#endif
}

bool has_dump()
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    return esp_core_dump_image_check() == ESP_OK;
#else
    return false;
#endif
}

void log_boot_summary()
{
    ESP_LOGI(kTag, "reset reason: %s", reset_reason_name(esp_reset_reason()));
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t s;
    if (!read_summary(s)) {
        ESP_LOGI(kTag, "no core dump stored");
        return;
    }
    ESP_LOGE(kTag, "=== previous crash (core dump present; GET /api/coredump, POST /api/coredump/clear) ===");
    ESP_LOGE(kTag, "  task=%s pc=0x%08lx exccause=%lu excvaddr=0x%08lx", s.exc_task,
             static_cast<unsigned long>(s.exc_pc), static_cast<unsigned long>(s.ex_info.exc_cause),
             static_cast<unsigned long>(s.ex_info.exc_vaddr));
    char line[16 * 11 + 1];
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < s.exc_bt_info.depth && i < 16; ++i) {
        n += std::snprintf(line + n, sizeof(line) - n, "0x%08lx ", static_cast<unsigned long>(s.exc_bt_info.bt[i]));
        if (n >= sizeof(line) - 12) break;
    }
    ESP_LOGE(kTag, "  backtrace%s: %s", s.exc_bt_info.corrupted ? " (corrupted)" : "", line);
    ESP_LOGE(kTag, "  decode: xtensa-esp32s3-elf-addr2line -pfiaC -e <elf> %s", line);
#endif
}

std::string summary_json()
{
    std::string out = "{\"reset_reason\":\"";
    out += reset_reason_name(esp_reset_reason());
    out += "\"";
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t s;
    if (!read_summary(s)) {
        out += ",\"present\":false}";
        return out;
    }
    char buf[96];
    out += ",\"present\":true,\"task\":\"";
    for (const char* p = s.exc_task; *p != '\0' && p < s.exc_task + sizeof(s.exc_task); ++p) {
        if (*p == '"' || *p == '\\') out += '\\';
        out += *p;
    }
    std::snprintf(buf, sizeof(buf), "\",\"pc\":\"0x%08lx\",\"exc_cause\":%lu,\"exc_vaddr\":\"0x%08lx\",\"bt\":[",
                  static_cast<unsigned long>(s.exc_pc), static_cast<unsigned long>(s.ex_info.exc_cause),
                  static_cast<unsigned long>(s.ex_info.exc_vaddr));
    out += buf;
    for (std::uint32_t i = 0; i < s.exc_bt_info.depth && i < 16; ++i) {
        std::snprintf(buf, sizeof(buf), "%s\"0x%08lx\"", i ? "," : "", static_cast<unsigned long>(s.exc_bt_info.bt[i]));
        out += buf;
    }
    out += "],\"bt_corrupted\":";
    out += s.exc_bt_info.corrupted ? "true" : "false";
    out += ",\"elf_sha256\":\"";
    // APP_ELF_SHA256_SZ 文字の文字列 (NUL 終端されている前提だが上限で切る)
    for (std::size_t i = 0; i < sizeof(s.app_elf_sha256) && s.app_elf_sha256[i] != 0; ++i) {
        out += static_cast<char>(s.app_elf_sha256[i]);
    }
    out += "\"}";
#else
    out += ",\"present\":false}";
#endif
    return out;
}

esp_err_t clear()
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    return esp_core_dump_image_erase();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

} // namespace stackchan::crash_report
