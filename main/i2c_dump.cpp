// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "i2c_dump.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "i2c_dump";

struct DumpRange {
    const char* name;
    std::uint8_t addr;
    std::uint8_t start_reg;
    std::uint16_t count;   // up to 256 (one full register page)
    std::uint32_t freq_hz; // bus clock for this chip
};

// HW-burned / power-on-default register values used as sanity beacons.
// If a chip is misbehaving we expect THESE to disagree with the dump first;
// catching that explicitly is more informative than eyeballing 100+ hex bytes.
struct KnownConst {
    const char* chip;
    std::uint8_t addr;
    std::uint8_t reg;
    std::uint8_t expected;
    const char* description;
};

constexpr KnownConst kKnownConsts[] = {
    // AXP2101: register 0x03 is the OTP/firmware version, hard-burned at the
    // factory. The chip always returns 0x4A; any other value means we're
    // either reading the wrong thing or the chip itself is sick.
    { "AXP2101", 0x34, 0x03, 0x4A, "OTP/FW version (HW constant)" },
    // AW9523 register 0x10 reads the chip ID (0x23 per datasheet).
    { "AW9523",  0x58, 0x10, 0x23, "chip ID" },
    // PY32 register 0x02 is the FW version. On our base it reads 0x41 (BSP
    // build); a healthy boot from the same PY32 should give the same value.
    { "PY32",    0x6F, 0x02, 0x41, "FW version" },
};

// One register read goes through M5Unified, which uses the same internal I2C
// driver M5 already has running. readRegister() returns false on NACK so we
// can distinguish "chip not present / not responding" from "register read 0".
bool try_read(std::uint8_t addr, std::uint8_t reg, std::uint8_t& out, std::uint32_t freq)
{
    return m5::In_I2C.readRegister(addr, reg, &out, 1, freq);
}

// Read the same register twice with a small bus-settle gap; report instability
// only when the two reads differ. Catches "stale data on SDA" failure modes
// where a NACK silently falls through and the master sees whatever value
// happened to be left on the line by the last write.
struct StableRead {
    bool ok;       // both reads ACK'd
    bool stable;   // both reads returned the same byte
    std::uint8_t value; // = first read; meaningful only when ok && stable
    std::uint8_t value2; // second read
};

StableRead try_read_x2(std::uint8_t addr, std::uint8_t reg, std::uint32_t freq)
{
    StableRead s{};
    std::uint8_t a = 0, b = 0;
    const bool ok_a = try_read(addr, reg, a, freq);
    esp_rom_delay_us(50); // give the chip time to release SDA before the next start
    const bool ok_b = try_read(addr, reg, b, freq);
    s.ok = ok_a && ok_b;
    s.stable = (a == b);
    s.value = a;
    s.value2 = b;
    return s;
}

void check_known_consts()
{
    for (const auto& kc : kKnownConsts) {
        StableRead s = try_read_x2(kc.addr, kc.reg, 100'000);
        if (!s.ok) {
            ESP_LOGW(kTag, "  %s @ 0x%02X reg 0x%02X (%s): NACK",
                     kc.chip, kc.addr, kc.reg, kc.description);
            continue;
        }
        if (!s.stable) {
            ESP_LOGE(kTag, "  %s @ 0x%02X reg 0x%02X (%s): UNSTABLE %02X / %02X (expect 0x%02X)",
                     kc.chip, kc.addr, kc.reg, kc.description, s.value, s.value2, kc.expected);
            continue;
        }
        if (s.value != kc.expected) {
            ESP_LOGE(kTag, "  %s @ 0x%02X reg 0x%02X (%s): MISMATCH got 0x%02X expect 0x%02X",
                     kc.chip, kc.addr, kc.reg, kc.description, s.value, kc.expected);
        } else {
            ESP_LOGI(kTag, "  %s @ 0x%02X reg 0x%02X (%s): OK 0x%02X",
                     kc.chip, kc.addr, kc.reg, kc.description, s.value);
        }
    }
}

void dump_one(const DumpRange& r)
{
    // Confirm the chip responds before scanning its register space. Reading
    // reg 0 once at the bus rate we'll use for the rest of the dump catches
    // "address not on the bus" / "bus arbitration lost" without burning ~64
    // failing transactions trying to dump 256 registers from nothing.
    std::uint8_t probe = 0;
    if (!try_read(r.addr, r.start_reg, probe, r.freq_hz)) {
        ESP_LOGW(kTag, "%s @ 0x%02X: no response (NACK on reg 0x%02X)",
                 r.name, r.addr, r.start_reg);
        return;
    }
    const std::uint8_t end = static_cast<std::uint8_t>(r.start_reg + r.count - 1);
    ESP_LOGI(kTag, "--- %s @ 0x%02X (regs 0x%02X..0x%02X, * = unstable across 2 reads) ---",
             r.name, r.addr, r.start_reg, end);

    constexpr int kPerLine = 16;
    // "  XX:" + 16 × (" XX*" = 4 chars) + NUL = 6 + 64 + 1 = 71. Pad to 96.
    char line[96];

    for (std::uint16_t row = 0; row < r.count; row += kPerLine) {
        const std::uint16_t row_end = std::min<std::uint16_t>(row + kPerLine, r.count);
        int n = std::snprintf(line, sizeof(line), "  %02X:",
                              static_cast<std::uint8_t>(r.start_reg + row));
        for (std::uint16_t off = row; off < row_end; ++off) {
            const std::uint8_t reg = static_cast<std::uint8_t>(r.start_reg + off);
            StableRead s = try_read_x2(r.addr, reg, r.freq_hz);
            if (!s.ok) {
                n += std::snprintf(line + n, sizeof(line) - n, " --");
            } else if (!s.stable) {
                // Show the first read value followed by '*' so the row still
                // aligns and the unstable cells stand out at a glance.
                n += std::snprintf(line + n, sizeof(line) - n, " %02X*", s.value);
            } else {
                n += std::snprintf(line + n, sizeof(line) - n, " %02X", s.value);
            }
            if (n >= static_cast<int>(sizeof(line))) break;
        }
        ESP_LOGI(kTag, "%s", line);
    }
}

} // namespace

void dump_internal_i2c_registers()
{
    // Single-chip ranges. Each chip is dumped completely (within its
    // documented register space) so the log is self-contained for future
    // diff'ing — better to over-include and let the human eyeball it than
    // miss the one register the bug touched.
    constexpr DumpRange ranges[] = {
        // AXP2101 PMIC. Documented range is roughly 0x00-0xA3 (status,
        // charging, DCDC/ALDO/BLDO/DLDO/CPUS-LDO enable, voltage setpoints,
        // ADC + IRQ status). The LCD backlight rail on CoreS3 is DLDO1
        // (reg 0x90 bit 7), voltage at reg 0x99 — the prime suspects for
        // "backlight off". Dumping through 0xA4 catches the entire PMU
        // control surface.
        { "AXP2101", 0x34, 0x00, 0xA4, 100'000 },
        // AW9523 IO expander on the CoreS3 main panel. Only 19 documented
        // registers (input/output/config/dir/intr/LED PWM) so the dump is
        // tiny but it tells us if any GPIO state is unexpected.
        { "AW9523",  0x58, 0x00, 0x13, 100'000 },
        // PY32 on the Stack-chan base. See docs/py32_ioexpander.md §2 — we
        // documented 0x00-0x40 as the used range (GPIO + ADC + PWM + LED).
        { "PY32",    0x6F, 0x00, 0x41, 100'000 },
    };

    ESP_LOGI(kTag, "internal-I2C register dump start");
    ESP_LOGI(kTag, "--- sanity beacons (HW constants) ---");
    check_known_consts();
    for (const auto& r : ranges) {
        dump_one(r);
    }

    // Probe: does AXP2101 accept register writes? If reg 0x90 is currently 0
    // (the "all LDOs disabled" failure mode), try setting it back to 0xBF and
    // immediately re-read. Outcomes:
    //   - read-back = 0xBF → chip is responsive, just needs M5Unified's init
    //                        re-run (software recovery possible).
    //   - read-back stays 0x00 → chip refusing writes, needs hard power cycle.
    // Only run the probe if the chip looks wrong to begin with — don't poke
    // a healthy AXP2101 unnecessarily.
    std::uint8_t cfwver = 0;
    if (try_read(0x34, 0x03, cfwver, 100'000) && cfwver != 0x4A) {
        std::uint8_t before = 0;
        std::uint8_t after = 0;
        const bool ok_before = try_read(0x34, 0x90, before, 100'000);
        ESP_LOGW(kTag, "AXP2101 write probe: reg 0x90 BEFORE = 0x%02X (read %s), writing 0xBF",
                 before, ok_before ? "OK" : "NACK");
        const bool ok_write = m5::In_I2C.writeRegister8(0x34, 0x90, 0xBF, 100'000);
        esp_rom_delay_us(200);
        const bool ok_after = try_read(0x34, 0x90, after, 100'000);
        ESP_LOGW(kTag, "AXP2101 write probe: write_ok=%d, reg 0x90 AFTER = 0x%02X (read %s)",
                 ok_write, after, ok_after ? "OK" : "NACK");
        if (after == 0xBF) {
            ESP_LOGW(kTag, "AXP2101 write probe: SUCCESS — chip accepts writes, "
                     "software recovery should work");
        } else if (after == before) {
            ESP_LOGW(kTag, "AXP2101 write probe: FAIL — write did not stick, "
                     "needs hard power cycle (battery + USB)");
        } else {
            ESP_LOGW(kTag, "AXP2101 write probe: PARTIAL — wrote 0xBF, got 0x%02X "
                     "(some bits accepted)", after);
        }
    }

    ESP_LOGI(kTag, "internal-I2C register dump end");
}

} // namespace stackchan::app
