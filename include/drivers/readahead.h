// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// A read-ahead window over a block device (src/drivers/readahead.c): a block
// of sectors read with one command and served from memory afterwards, for a
// transport where each command is expensive (USB mass storage). The caller
// owns the buffer and drops the window when a write overlaps it.
typedef struct readahead {
    bool valid;
    uint32_t lba;    // first sector held
    uint32_t count;  // sectors held
} readahead_t;

// The window to read for sector `lba` (below `sector_count`): `size` sectors
// aligned down to a multiple of `size`, cut short at the end of the device.
// Returns its first sector and stores its length in *count.
uint32_t readahead_window(uint32_t lba, uint32_t size, uint32_t sector_count, uint32_t *count);

// Does the window hold sector `lba`?
bool readahead_holds(const readahead_t *ra, uint32_t lba);

// Does the window share a sector with `count` sectors from `lba`?
bool readahead_overlaps(const readahead_t *ra, uint32_t lba, uint32_t count);

#ifdef __cplusplus
}
#endif
