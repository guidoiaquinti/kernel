// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// The ramdisk blob a kernel built with --ramdisk SIZE embeds
// (create-ramdisk-image.py writes it): a FAT32 volume's size and its non-zero
// 4 KiB runs, so a 1 GiB volume costs the kernel image only its used parts.
//
//   header   "KORARD1\0", u64 volume bytes, u32 run count, u32 reserved
//   runs     u64 offset, u64 length, then `length` bytes; offsets and lengths
//            are multiples of 4096, in increasing order, all little-endian

#define RAMDISK_RUN_ALIGN 4096u

// A range of free RAM, [base, base + size).
struct ram_range {
    uint64_t base;
    uint64_t size;
};

// Check a blob of `len` bytes and return the volume's size in *size: 0, or -1
// for a malformed blob (bad magic, runs out of order, unaligned, past the
// volume or the blob).
int ramdisk_image_check(const void *blob, size_t len, uint64_t *size);

// Lay out the volume of a checked blob (8-byte aligned) at `dst` (its size
// bytes, 4 KiB aligned): the runs' bytes, zeros everywhere else.
void ramdisk_image_unpack(const void *blob, void *dst);

// Where a volume of `size` bytes goes: the start of the first of the `count`
// free ranges (lowest first) that holds it once its start is aligned up to
// `align` (a power of two), or 0 if none does.
uint64_t ramdisk_place(const struct ram_range *free, unsigned count, uint64_t size,
                       uint64_t align);

// Remove [base, base + size) from `count` free ranges, splitting a range it
// falls inside; returns the new count, which stays at most `max` (a split that
// does not fit keeps the lower part only).
unsigned ramdisk_exclude(struct ram_range *ranges, unsigned count, unsigned max, uint64_t base,
                         uint64_t size);

// RAM in a Raspberry Pi with board revision `revision` (new-style revision
// codes give the size in bits 20-22; old-style boards have 256 or 512 MB), or
// 0 if unknown.
uint64_t ramdisk_pi_ram_bytes(uint32_t revision);

// Free RAM on a Raspberry Pi: from the end of the page pool to the end of the
// ARM's share of the first gigabyte (the GPU's memory follows it), and on a
// board with more than 1 GB the RAM above the first gigabyte, up to the
// peripherals at `device_base`. Returns the number of ranges (at most 2).
unsigned ramdisk_pi_ranges(uint64_t pool_end, uint64_t arm_end, uint64_t ram_bytes,
                           uint64_t device_base, struct ram_range out[2]);
