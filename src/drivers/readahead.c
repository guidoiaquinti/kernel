// SPDX-License-Identifier: GPL-3.0-or-later
#include "drivers/readahead.h"

uint32_t readahead_window(uint32_t lba, uint32_t size, uint32_t sector_count, uint32_t *count) {
    uint32_t base = lba - lba % size;
    uint32_t left = sector_count - base;
    *count = left < size ? left : size;
    return base;
}

bool readahead_holds(const readahead_t *ra, uint32_t lba) {
    return ra->valid && lba >= ra->lba && lba - ra->lba < ra->count;
}

bool readahead_overlaps(const readahead_t *ra, uint32_t lba, uint32_t count) {
    // 64-bit ends: a range may reach the top of the 32-bit sector space.
    return ra->valid && count > 0 && (uint64_t)lba < (uint64_t)ra->lba + ra->count &&
           (uint64_t)ra->lba < (uint64_t)lba + count;
}
