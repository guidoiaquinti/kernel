// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// SD card logic that does not touch hardware (src/drivers/sd.c, host-tested):
// decoding what the card reports and the SDHCI clock settings.

// Capacity in 512-byte sectors from a CSD register as an SDHCI controller
// stores an R2 response: the 120 bits above the CRC, shifted down by 8, with
// resp[0] holding the lowest 32. Handles CSD 1.0 (standard capacity) and 2.0
// (high and extended capacity); returns 0 for an unknown CSD structure.
uint64_t sd_csd_sectors(const uint32_t resp[4]);

// The SDHCI 3.0 10-bit divisor N for a card clock of at most `target_hz` from
// the controller's `base_hz`: the clock is base / (2 * N), or base itself for
// N = 0. Never faster than asked, unless that would need more than N's 10-bit
// maximum (1023).
unsigned sdhci_clock_divisor(uint32_t base_hz, uint32_t target_hz);

// The resulting card clock in Hz.
uint32_t sdhci_clock_hz(uint32_t base_hz, unsigned divisor);

// Where the controller's base clock came from (sd_base_clock).
enum sd_clock_source {
    SD_CLOCK_FIRMWARE,   // the firmware's current rate
    SD_CLOCK_MEASURED,   // the firmware's measured rate
    SD_CLOCK_CAPS,       // the controller's capabilities register
    SD_CLOCK_ASSUMED,    // nothing said: SD_ASSUMED_BASE_HZ
};
#define SD_ASSUMED_BASE_HZ 100000000u

// The base clock to divide from: the firmware's rate if it gives one, else its
// measured rate, else the controller's own claim (capabilities bits 15:8, in
// MHz), else SD_ASSUMED_BASE_HZ. *source says which.
uint32_t sd_base_clock(uint32_t firmware_hz, uint32_t measured_hz, uint32_t caps,
                       enum sd_clock_source *source);

// N in the clock control register's layout: its low 8 bits in bits 15:8, its
// top 2 bits in bits 7:6.
uint32_t sdhci_divisor_bits(unsigned divisor);
