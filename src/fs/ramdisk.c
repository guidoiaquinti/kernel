// SPDX-License-Identifier: GPL-3.0-or-later
//
// The embedded ramdisk (kernels built with ./build.sh --ramdisk SIZE): a FAT32
// volume of SIZE holding the shared userfs contents, laid out at boot in free
// RAM past the page pool, and read-write until the machine stops. The kernel
// image carries only the volume's non-zero parts (fs/ramdisk_image.h).

#include "fs/ramdisk.h"

#ifdef KORAOS_RAMDISK

#include "fs/ramdisk_image.h"
#include "mm/frame_alloc.h"
#include "lib/printf.h"
#ifdef KORAOS_VIRT
#include "platform/virt.h"
#else
#include "drivers/mailbox.h"
#include "peripherals/base.h"
#endif

extern const unsigned char _koraos_ramdisk_start[];
extern const unsigned char _koraos_ramdisk_end[];

#define MAX_RANGES 8
#define PLACE_ALIGN (2u * 1024 * 1024)  // a whole MMU block

static blkdev_t ramdisk;

static int ramdisk_read(blkdev_t *dev, uint32_t lba, uint32_t count, void *buf) {
    const uint8_t *src = (const uint8_t *)dev->ctx + (uint64_t)lba * BLK_SECTOR_SIZE;
    uint8_t *dst = buf;
    for (size_t i = 0; i < (size_t)count * BLK_SECTOR_SIZE; i++) {
        dst[i] = src[i];
    }
    return 0;
}

static int ramdisk_write(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf) {
    uint8_t *dst = (uint8_t *)dev->ctx + (uint64_t)lba * BLK_SECTOR_SIZE;
    const uint8_t *src = buf;
    for (size_t i = 0; i < (size_t)count * BLK_SECTOR_SIZE; i++) {
        dst[i] = src[i];
    }
    return 0;
}

static int ramdisk_flush(blkdev_t *dev) {
    (void)dev;
    return 0;  // RAM: nothing to flush
}

// Free RAM past the page pool, lowest first.
static unsigned free_ranges(struct ram_range out[MAX_RANGES]) {
    uint64_t pool_end = frame_alloc_pool_end();
#ifdef KORAOS_VIRT
    const struct virt_platform *machine = virt_platform_get();
    uint64_t ram_end = machine->ram_base + machine->ram_size;
    if (pool_end >= ram_end) {
        return 0;
    }
    unsigned n = 1;
    out[0] = (struct ram_range){pool_end, ram_end - pool_end};
    n = ramdisk_exclude(out, n, MAX_RANGES, machine->dtb_base, machine->dtb_size);
    for (unsigned r = 0; r < machine->reserved_count; r++) {
        n = ramdisk_exclude(out, n, MAX_RANGES, machine->reserved[r].base, machine->reserved[r].size);
    }
    return n;
#else
    uint32_t arm_base = 0, arm_size = 0;
    if (!mailbox_arm_memory(&arm_base, &arm_size)) {
        printf("[ramdisk] the firmware did not report the ARM's memory\n");
        return 0;
    }
    // The firmware's device tree may lie in the low range: Circle copied it
    // onto the heap at USB bring-up, and nothing reads the original after.
    uint64_t ram_bytes = ramdisk_pi_ram_bytes(mailbox_board_revision());
    // The peripherals end the MMU's RAM mapping (src/mm/mmu.c).
    uint64_t device_base = PBASE < 0xFC000000ul ? PBASE : 0xFC000000ul;
    return ramdisk_pi_ranges(pool_end, (uint64_t)arm_base + arm_size, ram_bytes, device_base, out);
#endif
}

blkdev_t *ramdisk_init(void) {
    const unsigned char *blob = _koraos_ramdisk_start;
    size_t len = (size_t)(_koraos_ramdisk_end - _koraos_ramdisk_start);
    uint64_t size;
    if (ramdisk_image_check(blob, len, &size) != 0) {
        printf("[ramdisk] the embedded image is damaged\n");
        return NULL;
    }
    struct ram_range ranges[MAX_RANGES];
    unsigned n = free_ranges(ranges);
    uint64_t base = ramdisk_place(ranges, n, size, PLACE_ALIGN);
    if (base == 0) {
        uint64_t largest = 0;
        for (unsigned i = 0; i < n; i++) {
            largest = ranges[i].size > largest ? ranges[i].size : largest;
        }
        printf("[ramdisk] %lu MB does not fit in free RAM (largest free range: %lu MB)\n",
               (unsigned long)(size >> 20), (unsigned long)(largest >> 20));
        return NULL;
    }
    ramdisk_image_unpack(blob, (void *)(uintptr_t)base);
    ramdisk = (blkdev_t){.read = ramdisk_read, .write = ramdisk_write, .flush = ramdisk_flush,
                         .sector_count = (uint32_t)(size / BLK_SECTOR_SIZE),
                         .ctx = (void *)(uintptr_t)base, .read_only = false};
    printf("[ramdisk] %lu MB at 0x%lx, read-write\n", (unsigned long)(size >> 20),
           (unsigned long)base);
    return &ramdisk;
}

#endif  // KORAOS_RAMDISK
