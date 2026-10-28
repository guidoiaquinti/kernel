// SPDX-License-Identifier: GPL-3.0-or-later
#include "fs/ramdisk_image.h"

static const uint8_t magic[8] = {'K', 'O', 'R', 'A', 'R', 'D', '1', '\0'};

#define HEADER_BYTES 24u
#define RUN_BYTES 16u

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int ramdisk_image_check(const void *blob, size_t len, uint64_t *size) {
    const uint8_t *p = blob;
    if (len < HEADER_BYTES) {
        return -1;
    }
    for (unsigned i = 0; i < sizeof(magic); i++) {
        if (p[i] != magic[i]) {
            return -1;
        }
    }
    uint64_t volume = rd64(p + 8);
    uint32_t count = rd32(p + 16);
    if (volume == 0 || volume % RAMDISK_RUN_ALIGN != 0) {
        return -1;
    }
    size_t at = HEADER_BYTES;
    uint64_t end = 0;  // the previous run's end: runs go up and never overlap
    for (uint32_t i = 0; i < count; i++) {
        if (len - at < RUN_BYTES) {
            return -1;
        }
        uint64_t offset = rd64(p + at);
        uint64_t length = rd64(p + at + 8);
        at += RUN_BYTES;
        if (offset % RAMDISK_RUN_ALIGN != 0 || length == 0 || length % RAMDISK_RUN_ALIGN != 0 ||
            offset < end || length > volume || offset > volume - length || length > len - at) {
            return -1;
        }
        end = offset + length;
        at += (size_t)length;
    }
    if (at != len) {
        return -1;
    }
    *size = volume;
    return 0;
}

// Runs and gaps are whole 4 KiB blocks and the destination is block aligned,
// so word loops do (zeroing a 1 GiB volume byte by byte takes too long).
static void zero_words(uint64_t *dst, uint64_t bytes) {
    for (uint64_t i = 0; i < bytes / 8; i++) {
        dst[i] = 0;
    }
}

// The blob is 8-byte aligned, and so is every run in it (24-byte header,
// 16-byte run headers, whole-block lengths).
static void copy_words(uint64_t *dst, const uint8_t *src, uint64_t bytes) {
    const uint64_t *words = (const uint64_t *)(const void *)src;
    for (uint64_t i = 0; i < bytes / 8; i++) {
        dst[i] = words[i];
    }
}

void ramdisk_image_unpack(const void *blob, void *dst) {
    const uint8_t *p = blob;
    uint8_t *out = dst;
    uint64_t volume = rd64(p + 8);
    uint32_t count = rd32(p + 16);
    size_t at = HEADER_BYTES;
    uint64_t done = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t offset = rd64(p + at);
        uint64_t length = rd64(p + at + 8);
        at += RUN_BYTES;
        zero_words((uint64_t *)(out + done), offset - done);
        copy_words((uint64_t *)(out + offset), p + at, length);
        at += (size_t)length;
        done = offset + length;
    }
    zero_words((uint64_t *)(out + done), volume - done);
}

uint64_t ramdisk_place(const struct ram_range *free, unsigned count, uint64_t size,
                       uint64_t align) {
    for (unsigned i = 0; i < count; i++) {
        uint64_t base = (free[i].base + align - 1) & ~(align - 1);
        if (base < free[i].base) {
            continue;  // aligning wrapped
        }
        uint64_t skipped = base - free[i].base;
        if (skipped <= free[i].size && size <= free[i].size - skipped && base != 0) {
            return base;
        }
    }
    return 0;
}

unsigned ramdisk_exclude(struct ram_range *ranges, unsigned count, unsigned max, uint64_t base,
                         uint64_t size) {
    uint64_t end = base + size;
    for (unsigned i = 0; i < count; i++) {
        uint64_t r_base = ranges[i].base;
        uint64_t r_end = r_base + ranges[i].size;
        if (size == 0 || end <= r_base || base >= r_end) {
            continue;
        }
        if (base > r_base && end < r_end) {  // inside: split in two
            ranges[i].size = base - r_base;
            if (count < max) {
                for (unsigned j = count; j > i + 1; j--) {
                    ranges[j] = ranges[j - 1];
                }
                ranges[i + 1] = (struct ram_range){end, r_end - end};
                count++;
                i++;
            }
        } else if (base > r_base) {  // covers the top
            ranges[i].size = base - r_base;
        } else if (end < r_end) {  // covers the bottom
            ranges[i] = (struct ram_range){end, r_end - end};
        } else {  // covers it all
            for (unsigned j = i; j + 1 < count; j++) {
                ranges[j] = ranges[j + 1];
            }
            count--;
            i--;
        }
    }
    return count;
}

uint64_t ramdisk_pi_ram_bytes(uint32_t revision) {
    if (revision & (1u << 23)) {
        unsigned code = (revision >> 20) & 7;
        return code <= 5 ? (uint64_t)256 << (20 + code) : 0;  // 256 MB .. 8 GB
    }
    // Old-style codes: the first Pis; only 0x0002 to 0x0006 have 256 MB.
    return revision == 0 ? 0 : revision <= 0x0006 ? (uint64_t)256 << 20 : (uint64_t)512 << 20;
}

unsigned ramdisk_pi_ranges(uint64_t pool_end, uint64_t arm_end, uint64_t ram_bytes,
                           uint64_t device_base, struct ram_range out[2]) {
    const uint64_t gib = (uint64_t)1 << 30;
    unsigned n = 0;
    if (arm_end > pool_end) {
        out[n++] = (struct ram_range){pool_end, arm_end - pool_end};
    }
    uint64_t high_end = ram_bytes < device_base ? ram_bytes : device_base;
    if (high_end > gib) {
        out[n++] = (struct ram_range){gib, high_end - gib};
    }
    return n;
}
