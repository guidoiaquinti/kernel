// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the embedded ramdisk's blob format and RAM placement
// (src/fs/ramdisk_image.c); create-ramdisk-image.py writes the blobs.
#include "fs/ramdisk_image.h"
#include "test.h"

void *aligned_alloc(size_t alignment, size_t size);
void free(void *ptr);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);

#define BLOCK RAMDISK_RUN_ALIGN

static uint64_t blob_words[(24 + 3 * (16 + 2 * BLOCK)) / 8 + 8];
static uint8_t *const blob = (uint8_t *)blob_words;
static size_t blob_len;

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void begin(uint64_t volume, uint32_t count) {
    memcpy(blob, "KORARD1", 8);
    put64(blob + 8, volume);
    put64(blob + 16, count);  // u32 count, u32 reserved (0)
    blob_len = 24;
}

// A run of `blocks` blocks at `offset`, block b filled with fill + b.
static void run(uint64_t offset, unsigned blocks, uint8_t fill) {
    put64(blob + blob_len, offset);
    put64(blob + blob_len + 8, (uint64_t)blocks * BLOCK);
    blob_len += 16;
    for (unsigned b = 0; b < blocks; b++) {
        memset(blob + blob_len, fill + b, BLOCK);
        blob_len += BLOCK;
    }
}

static void check_and_unpack(void) {
    // Two runs with a gap, a trailing gap, a 16-block volume.
    begin(16 * BLOCK, 2);
    run(0, 2, 0x10);
    run(5 * BLOCK, 1, 0x40);
    uint64_t size = 0;
    CHECK(ramdisk_image_check(blob, blob_len, &size) == 0 && size == 16 * BLOCK, "valid blob");
    uint8_t *volume = aligned_alloc(BLOCK, 16 * BLOCK);
    memset(volume, 0xee, 16 * BLOCK);  // RAM is not zero at boot
    ramdisk_image_unpack(blob, volume);
    bool ok = true;
    for (unsigned b = 0; b < 16; b++) {
        uint8_t want = b == 0 ? 0x10 : b == 1 ? 0x11 : b == 5 ? 0x40 : 0;
        for (unsigned i = 0; i < BLOCK; i++) {
            ok &= volume[b * BLOCK + i] == want;
        }
    }
    CHECK(ok, "runs copied, gaps and the tail zeroed");
    free(volume);

    begin(4 * BLOCK, 0);
    CHECK(ramdisk_image_check(blob, blob_len, &size) == 0 && size == 4 * BLOCK, "an empty volume");
}

static void malformed(void) {
    uint64_t size;
    begin(16 * BLOCK, 1);
    run(0, 1, 1);
    blob[0] = 'X';
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "bad magic");
    CHECK(ramdisk_image_check(blob, 10, &size) != 0, "short header");

    begin(16 * BLOCK + 512, 0);
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "volume not whole blocks");
    begin(0, 0);
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "empty volume size");

    begin(16 * BLOCK, 2);
    run(4 * BLOCK, 1, 1);
    run(2 * BLOCK, 1, 2);
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "runs out of order");

    begin(16 * BLOCK, 2);
    run(0, 2, 1);
    run(BLOCK, 1, 2);
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "overlapping runs");

    begin(16 * BLOCK, 1);
    run(100, 1, 1);
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "unaligned offset");

    begin(16 * BLOCK, 1);
    run(15 * BLOCK, 2, 1);
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "run past the volume");

    begin(16 * BLOCK, 1);
    run(0, 1, 1);
    put64(blob + 24 + 8, ~(uint64_t)4095);  // a huge length must not wrap
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "huge run length");

    begin(16 * BLOCK, 1);
    run(0, 2, 1);
    CHECK(ramdisk_image_check(blob, blob_len - BLOCK, &size) != 0, "run past the blob");
    begin(16 * BLOCK, 1);
    run(0, 1, 1);
    CHECK(ramdisk_image_check(blob, blob_len + 8, &size) != 0, "trailing bytes");
    begin(16 * BLOCK, 2);
    run(0, 1, 1);
    CHECK(ramdisk_image_check(blob, blob_len, &size) != 0, "fewer runs than counted");
}

static void place(void) {
    const uint64_t MB = 1024 * 1024;
    // A Pi 4 with 4 GB: free RAM past the page pool up to the GPU's memory,
    // then the 3 GB above the first gigabyte.
    struct ram_range pi4[] = {{0x02345000, 0x3b400000 - 0x02345000},
                              {0x40000000, 0xfc000000 - 0x40000000}};
    CHECK(ramdisk_place(pi4, 2, 16 * MB, 2 * MB) == 0x02400000, "small: low, aligned up");
    CHECK(ramdisk_place(pi4, 2, 1024 * MB, 2 * MB) == 0x40000000, "1G: above the first gigabyte");
    CHECK(ramdisk_place(pi4, 2, 3008 * MB, 2 * MB) == 0x40000000, "the whole high range");
    CHECK(ramdisk_place(pi4, 2, 3009 * MB, 2 * MB) == 0, "too big for any range");
    // A Pi 3: one low range only.
    struct ram_range pi3[] = {{0x02345000, 0x3b400000 - 0x02345000}};
    CHECK(ramdisk_place(pi3, 1, 1024 * MB, 2 * MB) == 0, "1G does not fit a Pi 3");
    CHECK(ramdisk_place(pi3, 1, 0x3b400000 - 0x02400000, 2 * MB) == 0x02400000, "exactly fits");
    CHECK(ramdisk_place(pi3, 1, 0x3b400000 - 0x02400000 + BLOCK, 2 * MB) == 0,
          "alignment counts against the range");
    struct ram_range tiny[] = {{0x1000, 0x1000}, {0x200000, 0x400000}};
    CHECK(ramdisk_place(tiny, 2, 0x2000, 2 * MB) == 0x200000, "a range lost to alignment is skipped");
    CHECK(ramdisk_place(0, 0, BLOCK, 2 * MB) == 0, "no free ranges");
}

static void exclude(void) {
    struct ram_range r[3] = {{0x1000, 0x9000}, {0x20000, 0x10000}};
    unsigned n = ramdisk_exclude(r, 2, 3, 0x3000, 0x1000);  // inside the first: split
    CHECK(n == 3 && r[0].base == 0x1000 && r[0].size == 0x2000 && r[1].base == 0x4000 &&
              r[1].size == 0x6000 && r[2].base == 0x20000,
          "split in two (%u ranges)", n);
    n = ramdisk_exclude(r, n, 3, 0x9000, 0x18000);  // the top of one, the bottom of the next
    CHECK(n == 3 && r[1].base == 0x4000 && r[1].size == 0x5000 && r[2].base == 0x21000 &&
              r[2].size == 0xf000,
          "top and bottom trimmed");
    n = ramdisk_exclude(r, n, 3, 0, 0x4000);  // the whole first range
    CHECK(n == 2 && r[0].base == 0x4000 && r[1].base == 0x21000, "a covered range goes");
    n = ramdisk_exclude(r, n, 3, 0x100000, 0x1000);
    CHECK(n == 2 && r[0].size == 0x5000 && r[1].size == 0xf000, "outside every range");
    n = ramdisk_exclude(r, n, 3, 0x22000, 0);
    CHECK(n == 2 && r[1].size == 0xf000, "an empty exclusion");
    struct ram_range one[1] = {{0x10000, 0x10000}};
    n = ramdisk_exclude(one, 1, 1, 0x14000, 0x1000);
    CHECK(n == 1 && one[0].base == 0x10000 && one[0].size == 0x4000, "no room to split: the lower part");
}

static void pi(void) {
    const uint64_t MB = 1024 * 1024;
    CHECK(ramdisk_pi_ram_bytes(0xa02082) == 1024 * MB, "Pi 3 Model B: 1 GB");
    CHECK(ramdisk_pi_ram_bytes(0xb03115) == 2048 * MB, "Pi 4 2 GB");
    CHECK(ramdisk_pi_ram_bytes(0xc03112) == 4096 * MB, "Pi 4 4 GB");
    CHECK(ramdisk_pi_ram_bytes(0xd03114) == 8192 * MB, "Pi 4 8 GB");
    CHECK(ramdisk_pi_ram_bytes(0x900092) == 512 * MB, "Pi Zero: 512 MB");
    CHECK(ramdisk_pi_ram_bytes(0x0002) == 256 * MB && ramdisk_pi_ram_bytes(0x000e) == 512 * MB,
          "old-style codes");
    CHECK(ramdisk_pi_ram_bytes(0) == 0 && ramdisk_pi_ram_bytes(0xe03115) == 0, "unknown");

    struct ram_range r[2];
    unsigned n = ramdisk_pi_ranges(0x02345000, 0x3b400000, 4096 * MB, 0xfc000000, r);
    CHECK(n == 2 && r[0].base == 0x02345000 && r[0].size == 0x3b400000 - 0x02345000 &&
              r[1].base == 0x40000000 && r[1].size == 0xbc000000,
          "Pi 4 4 GB: low, then up to the peripherals");
    n = ramdisk_pi_ranges(0x02345000, 0x3b400000, 2048 * MB, 0xfc000000, r);
    CHECK(n == 2 && r[1].size == 0x40000000, "Pi 4 2 GB: one high gigabyte");
    n = ramdisk_pi_ranges(0x02345000, 0x3b400000, 1024 * MB, 0x3f000000, r);
    CHECK(n == 1 && r[0].base == 0x02345000, "Pi 3: no high range");
    n = ramdisk_pi_ranges(0x02345000, 0, 0, 0x3f000000, r);
    CHECK(n == 0, "the firmware gave nothing");
}

TEST_MAIN(check_and_unpack, malformed, place, exclude, pi)
