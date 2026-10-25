// SPDX-License-Identifier: GPL-3.0-or-later
//
// SD card logic without hardware access. See drivers/sd.h.

#include "drivers/sd.h"

// Bits [hi:lo] of a CSD stored as SDHCI keeps an R2 response. CSD bit b sits
// at bit b - 8 of resp[] (the CRC byte is dropped).
static uint32_t csd_bits(const uint32_t resp[4], unsigned hi, unsigned lo) {
    uint32_t value = 0;
    for (unsigned b = hi + 1; b-- > lo;) {
        unsigned r = b - 8;
        value = (value << 1) | ((resp[r / 32] >> (r % 32)) & 1u);
    }
    return value;
}

uint64_t sd_csd_sectors(const uint32_t resp[4]) {
    switch (csd_bits(resp, 127, 126)) {
    case 0: {  // CSD 1.0: C_SIZE, C_SIZE_MULT and READ_BL_LEN
        uint64_t c_size = csd_bits(resp, 73, 62);
        unsigned mult = csd_bits(resp, 49, 47);
        unsigned bl_len = csd_bits(resp, 83, 80);
        uint64_t bytes = (c_size + 1) << (mult + 2 + bl_len);
        return bytes / 512;
    }
    case 1: {  // CSD 2.0: (C_SIZE + 1) * 512 KB
        uint64_t c_size = csd_bits(resp, 69, 48);
        return (c_size + 1) * 1024;
    }
    default:
        return 0;
    }
}

unsigned sdhci_clock_divisor(uint32_t base_hz, uint32_t target_hz) {
    if (target_hz == 0) {
        return 1023;
    }
    if (base_hz <= target_hz) {
        return 0;
    }
    uint32_t n = (base_hz + 2 * target_hz - 1) / (2 * target_hz);
    return n > 1023 ? 1023 : n;
}

uint32_t sdhci_clock_hz(uint32_t base_hz, unsigned divisor) {
    return divisor == 0 ? base_hz : base_hz / (2 * divisor);
}

uint32_t sd_base_clock(uint32_t firmware_hz, uint32_t measured_hz, uint32_t caps,
                       enum sd_clock_source *source) {
    uint32_t caps_mhz = (caps >> 8) & 0xffu;
    if (firmware_hz != 0) {
        *source = SD_CLOCK_FIRMWARE;
        return firmware_hz;
    }
    if (measured_hz != 0) {
        *source = SD_CLOCK_MEASURED;
        return measured_hz;
    }
    if (caps_mhz != 0) {
        *source = SD_CLOCK_CAPS;
        return caps_mhz * 1000000u;
    }
    *source = SD_CLOCK_ASSUMED;
    return SD_ASSUMED_BASE_HZ;
}

uint32_t sdhci_divisor_bits(unsigned divisor) {
    return ((divisor & 0xffu) << 8) | (((divisor >> 8) & 3u) << 6);
}
