// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the SD card logic (src/drivers/sd.c): CSD capacity decoding
// and the SDHCI clock divisor.
#include "drivers/sd.h"
#include "test.h"

// Put `value` into CSD bits [hi:lo], stored as SDHCI keeps an R2 response
// (CSD bit b at bit b - 8 of resp[]).
static void csd_set(uint32_t resp[4], unsigned hi, unsigned lo, uint32_t value) {
    for (unsigned b = lo; b <= hi; b++) {
        unsigned r = b - 8;
        uint32_t bit = (value >> (b - lo)) & 1u;
        resp[r / 32] = (resp[r / 32] & ~(1u << (r % 32))) | (bit << (r % 32));
    }
}

static void csd_v1(void) {
    // A 2 GB standard-capacity card: 1024-byte blocks, C_SIZE 4095, C_SIZE_MULT 7.
    uint32_t resp[4] = {0};
    csd_set(resp, 127, 126, 0);
    csd_set(resp, 83, 80, 10);
    csd_set(resp, 73, 62, 4095);
    csd_set(resp, 49, 47, 7);
    CHECK(sd_csd_sectors(resp) == 4194304, "2 GB SDSC: %lu sectors",
          (unsigned long)sd_csd_sectors(resp));
    // A 64 MB card: 512-byte blocks, C_SIZE 1959, C_SIZE_MULT 5.
    uint32_t small[4] = {0};
    csd_set(small, 83, 80, 9);
    csd_set(small, 73, 62, 1959);
    csd_set(small, 49, 47, 5);
    CHECK(sd_csd_sectors(small) == 1960u * 128, "64 MB SDSC: %lu sectors",
          (unsigned long)sd_csd_sectors(small));
    // C_SIZE straddles resp[1] and resp[2]: neighbouring bits must not leak in.
    uint32_t noisy[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0x003fffffu};
    csd_set(noisy, 127, 126, 0);
    csd_set(noisy, 83, 80, 9);
    csd_set(noisy, 73, 62, 1);
    csd_set(noisy, 49, 47, 0);
    CHECK(sd_csd_sectors(noisy) == 2 * 4, "neighbouring bits: %lu sectors",
          (unsigned long)sd_csd_sectors(noisy));
}

static void csd_v2(void) {
    // A "4 GB" SDHC card: C_SIZE 7579 -> 7580 * 512 KB.
    uint32_t resp[4] = {0};
    csd_set(resp, 127, 126, 1);
    csd_set(resp, 69, 48, 7579);
    CHECK(sd_csd_sectors(resp) == 7580u * 1024, "SDHC: %lu sectors",
          (unsigned long)sd_csd_sectors(resp));
    // A 128 GB SDXC card: past 32 bits of bytes, and of 2^32 / 512 * 512.
    uint32_t xc[4] = {0xffffffffu, 0, 0, 0};
    csd_set(xc, 127, 126, 1);
    csd_set(xc, 69, 48, 0x3b9ff);
    CHECK(sd_csd_sectors(xc) == 0x3ba00ull * 1024, "SDXC: %lu sectors",
          (unsigned long)sd_csd_sectors(xc));
    // Reserved structures are refused.
    uint32_t odd[4] = {0};
    csd_set(odd, 127, 126, 2);
    CHECK(sd_csd_sectors(odd) == 0, "CSD structure 2 is unknown");
}

static void clock_divisor(void) {
    struct {
        uint32_t base, target, divisor, hz;
    } cases[] = {
        {50000000, 400000, 63, 396825},     // QEMU's EMMC clock, identification
        {50000000, 25000000, 1, 25000000},  // ... and default speed
        {200000000, 25000000, 4, 25000000},
        {100000000, 400000, 125, 400000},
        {41666666, 25000000, 1, 20833333},  // never faster than asked
        {20000000, 25000000, 0, 20000000},  // a slow base clock is used as is
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned n = sdhci_clock_divisor(cases[i].base, cases[i].target);
        uint32_t hz = sdhci_clock_hz(cases[i].base, n);
        CHECK(n == cases[i].divisor && hz == cases[i].hz, "case %u: N %u, %u Hz", i, n,
              (unsigned)hz);
        CHECK(hz <= cases[i].target || n == 0, "case %u: faster than asked", i);
    }
    CHECK(sdhci_clock_divisor(1000000000u, 400000) == 1023, "capped at 10 bits");
    CHECK(sdhci_clock_divisor(50000000, 0) == 1023, "zero target: slowest");
    CHECK(sdhci_divisor_bits(0x123) == 0x2340, "N 0x123: low byte in 15:8, top bits in 7:6");
    CHECK(sdhci_divisor_bits(63) == 0x3f00, "N 63");
    CHECK(sdhci_divisor_bits(1023) == 0xffc0, "N 1023 fills the field");
}

static void base_clock(void) {
    enum sd_clock_source source;
    uint32_t caps = 0x52134b4;  // QEMU's capabilities register: 52 MHz in bits 15:8
    CHECK(sd_base_clock(200000000, 0, caps, &source) == 200000000 && source == SD_CLOCK_FIRMWARE,
          "the firmware's rate comes first");
    CHECK(sd_base_clock(0, 199999000, caps, &source) == 199999000 && source == SD_CLOCK_MEASURED,
          "then its measurement");
    CHECK(sd_base_clock(0, 0, caps, &source) == 52000000 && source == SD_CLOCK_CAPS,
          "then the controller's capabilities");
    CHECK(sd_base_clock(0, 0, 0xffff00ffu, &source) == SD_ASSUMED_BASE_HZ &&
              source == SD_CLOCK_ASSUMED,
          "a zero capabilities field means unknown");
}

TEST_MAIN(csd_v1, csd_v2, clock_divisor, base_clock)
