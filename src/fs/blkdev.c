#include "fs/blkdev.h"
#ifdef KORAOS_VIRTIO_BLK
#include "drivers/virtio_blk.h"
#endif
#if defined(KORAOS_VIRTIO_BLK) || defined(KORAOS_SD) || defined(KORAOS_USB_STORAGE) || \
    defined(KORAOS_RAMDISK)
#include "mini_uart.h"
#endif
#ifdef KORAOS_RAMDISK
#include "fs/ramdisk.h"
#endif
#ifdef KORAOS_SD
#include "drivers/sdhci.h"
#endif
#ifdef KORAOS_USB_STORAGE
#include "drivers/usb_storage.h"
#endif

static blkdev_device_t devices[BLKDEV_MAX_DEVICES];
static blkdev_partition_t partitions[BLKDEV_MAX_PARTITIONS];
static unsigned device_count, partition_count;
static blkdev_t *active;
static bool root_search;

static uint32_t rd16(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int check_range(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf) {
    if (!dev) return BLK_ERR_NODEV;
    if ((uint64_t)lba + count > dev->sector_count) return BLK_ERR_RANGE;
    if (count && !buf) return BLK_ERR_INVALID;
    return 0;
}

int blkdev_read(blkdev_t *dev, uint32_t lba, uint32_t count, void *buf) {
    int rc = check_range(dev, lba, count, buf);
    if (rc || !count) return rc;
    return dev->read ? dev->read(dev, lba, count, buf) : BLK_ERR_UNSUPPORTED;
}

int blkdev_write(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf) {
    int rc = check_range(dev, lba, count, buf);
    if (rc || !count) return rc;
    if (dev->read_only) return BLK_ERR_RO;
    return dev->write ? dev->write(dev, lba, count, buf) : BLK_ERR_UNSUPPORTED;
}

int blkdev_flush(blkdev_t *dev) {
    if (!dev) return BLK_ERR_NODEV;
    if (dev->read_only) return BLK_ERR_RO;
    return dev->flush ? dev->flush(dev) : BLK_ERR_UNSUPPORTED;
}

static int partition_read(blkdev_t *dev, uint32_t lba, uint32_t count, void *buf) {
    blkdev_partition_t *part = dev->ctx;
    int rc = check_range(dev, lba, count, buf);
    if (rc || !count) return rc;
    return blkdev_read(part->device->backend, part->start_lba + lba, count, buf);
}

static int partition_write(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf) {
    blkdev_partition_t *part = dev->ctx;
    int rc = check_range(dev, lba, count, buf);
    if (rc || !count) return rc;
    return blkdev_write(part->device->backend, part->start_lba + lba, count, buf);
}

static int partition_flush(blkdev_t *dev) {
    blkdev_partition_t *part = dev->ctx;
    return blkdev_flush(part->device->backend);
}

static void add_partition(blkdev_device_t *device, uint32_t start, uint32_t count,
                          unsigned index, uint8_t type) {
    blkdev_partition_t *part = &partitions[partition_count++];
    *part = (blkdev_partition_t){
        .dev = {.read = partition_read,
                .write = device->backend->write ? partition_write : NULL,
                .flush = device->backend->flush ? partition_flush : NULL,
                .sector_count = count, .ctx = part,
                .read_only = device->backend->read_only},
        .device = device, .start_lba = start, .mbr_index = index, .mbr_type = type,
    };
    device->partition_count++;
}

// FAT32 BPBs use a jump instruction and carry geometry, unlike an MBR. Ignore
// boot code bytes in the partition-table region of a bare filesystem.
static bool bare_fat32(const uint8_t *sector) {
    uint32_t spc = sector[13];
    return (sector[0] == 0xe9 || (sector[0] == 0xeb && sector[2] == 0x90)) &&
           rd16(sector + 11) == BLK_SECTOR_SIZE && spc && !(spc & (spc - 1)) &&
           spc <= 128 && rd16(sector + 14) && sector[16] &&
           !rd16(sector + 17) && !rd16(sector + 22) && rd32(sector + 36) &&
           rd32(sector + 44) >= 2;
}

static bool fat32_type(uint8_t type) {
    return type == 0x0b || type == 0x0c || type == 0x1b || type == 0x1c;
}

static int probe(blkdev_device_t *device) {
    uint8_t sector[BLK_SECTOR_SIZE];
    int rc = blkdev_read(device->backend, 0, 1, sector);
    if (rc) return rc;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return BLK_ERR_INVALID;
    if (bare_fat32(sector)) {
        uint32_t count = rd16(sector + 19);
        if (!count) count = rd32(sector + 32);
        if (!count || count > device->backend->sector_count) return BLK_ERR_RANGE;
        add_partition(device, 0, count, 0, 0);
        return 0;
    }

    // Validate every occupied primary, including unsupported filesystems, before
    // publishing any FAT views. This prevents partial discovery of damaged MBRs.
    uint32_t starts[4], counts[4];
    uint8_t types[4];
    unsigned found = 0;
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *entry = sector + 446 + i * 16;
        types[i] = entry[4];
        starts[i] = rd32(entry + 8);
        counts[i] = rd32(entry + 12);
        if (!types[i]) {
            for (unsigned j = 0; j < 16; j++) {
                if (entry[j]) return BLK_ERR_INVALID;
            }
            continue;
        }
        if (entry[0] != 0 && entry[0] != 0x80) return BLK_ERR_INVALID;
        if (!starts[i] || !counts[i] ||
            (uint64_t)starts[i] + counts[i] > device->backend->sector_count) {
            return BLK_ERR_RANGE;
        }
        for (unsigned j = 0; j < i; j++) {
            if (types[j] && (uint64_t)starts[i] < (uint64_t)starts[j] + counts[j] &&
                (uint64_t)starts[j] < (uint64_t)starts[i] + counts[i]) {
                return BLK_ERR_INVALID;
            }
        }
        if (types[i] == 0xee || types[i] == 0x05 || types[i] == 0x0f || types[i] == 0x85) {
            return BLK_ERR_UNSUPPORTED;
        }
        if (fat32_type(types[i])) found++;
    }
    if (!found) return BLK_ERR_UNSUPPORTED;
    for (unsigned i = 0; i < 4; i++) {
        if (fat32_type(types[i])) add_partition(device, starts[i], counts[i], i + 1, types[i]);
    }
    return 0;
}

void blkdev_registry_reset(void) {
    device_count = partition_count = 0;
    active = NULL;
    root_search = false;
}

int blkdev_register(blkdev_t *backend, blkdev_type_t type) {
    if (!backend || !backend->read || !backend->sector_count ||
        type < BLKDEV_RAMDISK || type > BLKDEV_USB) return BLK_ERR_INVALID;
    for (unsigned i = 0; i < device_count; i++) {
        if (devices[i].backend == backend) return BLK_ERR_BUSY;
    }
    if (device_count == BLKDEV_MAX_DEVICES) return BLK_ERR_BUSY;
    blkdev_device_t *device = &devices[device_count++];
    *device = (blkdev_device_t){.backend = backend, .type = type,
                              .first_partition = partition_count};
    device->probe_error = probe(device);
    return device->probe_error;
}

unsigned blkdev_device_count(void) { return device_count; }
const blkdev_device_t *blkdev_device_get(unsigned index) {
    return index < device_count ? &devices[index] : NULL;
}
unsigned blkdev_partition_count(void) { return partition_count; }
const blkdev_partition_t *blkdev_partition_get(unsigned index) {
    return index < partition_count ? &partitions[index] : NULL;
}

blkdev_t *blkdev_partition_io(unsigned index) {
    return index < partition_count ? &partitions[index].dev : NULL;
}

void blkdev_init(void) {
    blkdev_registry_reset();
#ifdef KORAOS_RAMDISK
    blkdev_t *rd = ramdisk_init();
    if (rd != NULL) {
        unsigned first = partition_count;
        if (blkdev_register(rd, BLKDEV_RAMDISK) == 0) {
            active = &partitions[first].dev;
        } else {
            uart_puts("[blkdev] ramdisk has no FAT32 volume\r\n");
        }
    }
#endif
#ifdef KORAOS_SD
    // The card's FAT32 partitions become volumes next to the ramdisk.
    bool sd_present = false;
    blkdev_t *sd = sdhci_init(&sd_present);
    if (sd != NULL && blkdev_register(sd, BLKDEV_SD) != 0) {
        uart_puts("[blkdev] SD card has no FAT32 volume\r\n");
    }
#endif
#ifdef KORAOS_USB_STORAGE
    // USB drives found while Circle enumerated (circle_usb_init runs first).
    for (unsigned i = 0; i < usb_storage_count(); i++) {
        if (blkdev_register(usb_storage_device(i), BLKDEV_USB) != 0) {
            uart_puts("[blkdev] USB drive has no FAT32 volume\r\n");
        }
    }
#endif
#ifdef KORAOS_VIRTIO_BLK
    bool present = false;
    blkdev_t *disk = virtio_blk_init(&present);
    if (present) {
        // A configured disk is the root, even a broken one: no fallback.
        active = NULL;
        if (disk) {
            unsigned first = partition_count;
            if (!blkdev_register(disk, BLKDEV_VIRTIO)) {
                active = &partitions[first].dev;
            } else {
                uart_puts("[blkdev] VirtIO partition probe failed; no root device\r\n");
            }
        }
        // Keep the established backend diagnostic for smoke/failure checks.
        uart_puts(disk ? "[blkdev] using VirtIO disk\r\n" :
                        "[blkdev] configured VirtIO disk failed; no root device\r\n");
        return;
    }
    uart_puts(active ? "[blkdev] no VirtIO disk; using the ramdisk\r\n" :
                       "[blkdev] no VirtIO disk\r\n");
#endif
    // Without a ramdisk the boot volume is the first one with /bin/init.
    root_search = active == NULL;
}

blkdev_t *blkdev_root(void) { return active; }
bool blkdev_root_search(void) { return root_search; }

int blk_read(uint32_t lba, uint32_t count, void *buf) {
    return blkdev_read(active, lba, count, buf);
}
int blk_write(uint32_t lba, uint32_t count, const void *buf) {
    return blkdev_write(active, lba, count, buf);
}
int blk_flush(void) { return blkdev_flush(active); }
bool blk_is_read_only(void) { return !active || active->read_only; }
uint32_t blk_sector_count(void) { return active ? active->sector_count : 0; }
