#pragma once

#include "common.h"

#define BLK_SECTOR_SIZE 512u
#define BLKDEV_MAX_DEVICES 8u
#define BLKDEV_MAX_PARTITIONS (BLKDEV_MAX_DEVICES * 4u)

#define BLK_ERR_NODEV (-1)
#define BLK_ERR_RANGE (-2)
#define BLK_ERR_IO (-3)
#define BLK_ERR_RO (-4)
#define BLK_ERR_UNSUPPORTED (-5)
#define BLK_ERR_INVALID (-6)
#define BLK_ERR_BUSY (-7)

// Backends expose physical, 512-byte sectors. Registry partition views expose
// the same interface with partition-relative LBAs. Backends must remain alive
// for the lifetime of the registry; registration does not copy backend state.
typedef struct blkdev {
    int (*read)(struct blkdev *dev, uint32_t lba, uint32_t count, void *buf);
    uint32_t sector_count;
    void *ctx;
    int (*write)(struct blkdev *dev, uint32_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blkdev *dev);
    bool read_only;
} blkdev_t;

typedef enum blkdev_type {
    BLKDEV_RAMDISK,
    BLKDEV_VIRTIO,
    BLKDEV_SD,
    BLKDEV_USB,
} blkdev_type_t;

typedef struct blkdev_device {
    blkdev_t *backend;
    blkdev_type_t type;
    unsigned first_partition;
    unsigned partition_count;
    int probe_error;
} blkdev_device_t;

typedef struct blkdev_partition {
    blkdev_t dev;
    const blkdev_device_t *device;
    uint32_t start_lba;
    unsigned mbr_index; // 1..4 for an MBR primary, 0 for a bare FAT32 volume
    uint8_t mbr_type;
} blkdev_partition_t;

// Boot/test reset only. The kernel serializes all filesystem operations.
void blkdev_registry_reset(void);
// Registers a physical backend and probes bare FAT32 or primary MBR FAT32
// partitions. Invalid media stays registered with probe_error and no views.
// GPT and extended partitions are unsupported. Duplicate backends are rejected.
int blkdev_register(blkdev_t *backend, blkdev_type_t type);
unsigned blkdev_device_count(void);
const blkdev_device_t *blkdev_device_get(unsigned index);
unsigned blkdev_partition_count(void);
const blkdev_partition_t *blkdev_partition_get(unsigned index);
blkdev_t *blkdev_partition_io(unsigned index);

// All transport calls pass through bounds and argument checks, including views.
int blkdev_read(blkdev_t *dev, uint32_t lba, uint32_t count, void *buf);
int blkdev_write(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf);
int blkdev_flush(blkdev_t *dev);

// Register the ramdisk (if built with one), the SD card, USB drives and a
// VirtIO disk. The root is a configured VirtIO disk (a broken one never falls
// back to anything else), else the ramdisk; with neither, blkdev_root() is NULL
// and blkdev_root_search() true: the boot volume is the first volume that has
// /bin/init (fs_mount_registered_search). Legacy blk_* calls use the root.
void blkdev_init(void);
blkdev_t *blkdev_root(void);
bool blkdev_root_search(void);
int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);
int blk_flush(void);
bool blk_is_read_only(void);
uint32_t blk_sector_count(void);
