// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#include "fs/blkdev.h"

static uint8_t image[128 * BLK_SECTOR_SIZE];
static blkdev_t backend;
static unsigned reads, writes, flushes;
static uint32_t last_lba, last_count;
static int read_error;

static void clear_bytes(void *buf, size_t count) {
    uint8_t *p = buf;
    for (size_t i = 0; i < count; i++) p[i] = 0;
}
static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}
static int fake_read(blkdev_t *dev, uint32_t lba, uint32_t count, void *buf) {
    (void)dev;
    reads++;
    last_lba = lba;
    last_count = count;
    if (read_error) return read_error;
    CHECK((uint64_t)lba + count <= 128, "backend read remains in fixture");
    if ((uint64_t)lba + count > 128) return BLK_ERR_RANGE;
    uint8_t *dst = buf;
    for (size_t i = 0; i < (size_t)count * BLK_SECTOR_SIZE; i++) {
        dst[i] = image[(size_t)lba * BLK_SECTOR_SIZE + i];
    }
    return 0;
}
static int fake_write(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf) {
    (void)dev;
    writes++;
    last_lba = lba;
    last_count = count;
    CHECK((uint64_t)lba + count <= 128, "backend write remains in fixture");
    if ((uint64_t)lba + count > 128) return BLK_ERR_RANGE;
    const uint8_t *src = buf;
    for (size_t i = 0; i < (size_t)count * BLK_SECTOR_SIZE; i++) {
        image[(size_t)lba * BLK_SECTOR_SIZE + i] = src[i];
    }
    return 0;
}
static int fake_flush(blkdev_t *dev) {
    CHECK(dev == &backend, "flush uses physical backend");
    flushes++;
    return 0;
}
static void setup(void) {
    blkdev_registry_reset();
    clear_bytes(image, sizeof(image));
    image[510] = 0x55;
    image[511] = 0xaa;
    backend = (blkdev_t){.read = fake_read, .write = fake_write, .flush = fake_flush,
                        .sector_count = 128};
    reads = writes = flushes = 0;
    read_error = 0;
}
static void entry(unsigned index, uint8_t type, uint32_t start, uint32_t count) {
    uint8_t *p = image + 446 + index * 16;
    p[4] = type;
    put32(p + 8, start);
    put32(p + 12, count);
}
static blkdev_t *view(unsigned index) {
    return blkdev_partition_io(index);
}
static void check_rejected(int expected) {
    CHECK(blkdev_register(&backend, BLKDEV_SD) == expected, "probe rejects (%d)", expected);
    CHECK(blkdev_device_count() == 1 && blkdev_partition_count() == 0,
          "failed physical device retained without partial views");
    CHECK(blkdev_device_get(0)->probe_error == expected, "probe error retained");
}
static void bare_volume(void) {
    setup();
    image[0] = 0xeb;
    image[2] = 0x90;
    image[12] = 2;
    image[13] = 1;
    image[14] = 32;
    image[16] = 2;
    put32(image + 32, 100);
    put32(image + 36, 1);
    put32(image + 44, 2);
    // Bare FAT boot code can occupy the region that an MBR uses for entries.
    image[446] = 0xff;
    CHECK(blkdev_register(&backend, BLKDEV_RAMDISK) == 0, "bare FAT32 recognized");
    const blkdev_partition_t *p = blkdev_partition_get(0);
    CHECK(p && p->mbr_index == 0 && p->start_lba == 0 && p->dev.sector_count == 100,
          "bare volume bounded by declared size");
    CHECK(p->device->type == BLKDEV_RAMDISK, "backend type retained");
    CHECK(blkdev_partition_get(1) == NULL && blkdev_device_get(1) == NULL,
          "enumeration is bounded");
    put32(image + 32, 0);
    image[19] = 100;
    blkdev_registry_reset();
    CHECK(blkdev_register(&backend, BLKDEV_RAMDISK) == 0 &&
          view(0)->sector_count == 100, "mtools small FAT32 uses TotalSectors16");
    image[19] = 0;
    put32(image + 32, 129);
    blkdev_registry_reset();
    check_rejected(BLK_ERR_RANGE);
}
static void mbr_and_io(void) {
    setup();
    entry(0, 0x83, 1, 8); // Linux filesystem is safely ignored.
    entry(1, 0x0b, 16, 32);
    entry(2, 0x1c, 64, 64);
    CHECK(blkdev_register(&backend, BLKDEV_VIRTIO) == 0, "mixed MBR supported");
    CHECK(blkdev_partition_count() == 2, "only FAT32 primaries published");
    CHECK(blkdev_partition_get(0)->mbr_index == 2 &&
          blkdev_partition_get(1)->mbr_index == 3, "original primary indices retained");
    uint8_t buf[2 * BLK_SECTOR_SIZE];
    clear_bytes(buf, sizeof(buf));
    image[16 * BLK_SECTOR_SIZE] = 42;
    CHECK(blkdev_read(view(0), 0, 1, buf) == 0 && buf[0] == 42 && last_lba == 16,
          "partition reads translate relative LBA");
    buf[0] = 99;
    CHECK(blkdev_write(view(1), 63, 1, buf) == 0 && last_lba == 127 && last_count == 1,
          "last sector write translates");
    CHECK(image[127 * BLK_SECTOR_SIZE] == 99, "translated data stored");
    CHECK(blkdev_flush(view(0)) == 0 && flushes == 1, "flush forwarded");
    unsigned before_reads = reads, before_writes = writes;
    CHECK(blkdev_read(view(0), 31, 2, buf) == BLK_ERR_RANGE, "read cannot cross partition");
    CHECK(blkdev_write(view(0), 0xffffffffu, 2, buf) == BLK_ERR_RANGE,
          "addition overflow cannot bypass write bounds");
    CHECK(blkdev_read(view(0), 0, 1, NULL) == BLK_ERR_INVALID, "null nonempty read rejected");
    CHECK(blkdev_write(view(0), 0, 1, NULL) == BLK_ERR_INVALID, "null write rejected");
    CHECK(blkdev_read(view(0), 32, 0, NULL) == 0, "zero read at end is no-op");
    CHECK(blkdev_write(view(0), 32, 0, NULL) == 0, "zero write at end is no-op");
    CHECK(blkdev_read(view(0), 33, 0, NULL) == BLK_ERR_RANGE, "zero read past end rejected");
    CHECK(reads == before_reads && writes == before_writes, "rejected/no-op I/O bypasses backend");
    backend.read_only = true;
    CHECK(blkdev_write(view(0), 0, 1, buf) == BLK_ERR_RO, "physical RO blocks existing view");
    CHECK(blkdev_flush(view(0)) == BLK_ERR_RO, "physical RO blocks flush");
    backend.read_only = false;
    backend.write = NULL;
    backend.flush = NULL;
    CHECK(blkdev_write(view(0), 0, 1, buf) == BLK_ERR_UNSUPPORTED, "missing write op propagated");
    CHECK(blkdev_flush(view(0)) == BLK_ERR_UNSUPPORTED, "missing flush op propagated");
    CHECK(blkdev_read(NULL, 0, 0, NULL) == BLK_ERR_NODEV, "absent device rejected");
    CHECK(blkdev_write(NULL, 0, 0, NULL) == BLK_ERR_NODEV, "absent write device rejected");
    CHECK(blkdev_flush(NULL) == BLK_ERR_NODEV, "absent flush device rejected");
}
static void malformed_media(void) {
    setup(); entry(0, 0x0c, 1, 127); image[511] = 0;
    check_rejected(BLK_ERR_INVALID);
    setup(); entry(0, 0x0c, 0, 8); check_rejected(BLK_ERR_RANGE);
    setup(); entry(0, 0x0c, 1, 0); check_rejected(BLK_ERR_RANGE);
    setup(); entry(0, 0x0c, 120, 9); check_rejected(BLK_ERR_RANGE);
    setup(); entry(0, 0x0c, 0xfffffff0u, 32); check_rejected(BLK_ERR_RANGE);
    setup(); entry(0, 0x0c, 1, 32); entry(1, 0x83, 32, 5);
    check_rejected(BLK_ERR_INVALID);
    setup(); entry(0, 0x0c, 1, 32); entry(1, 0x0c, 1, 32);
    check_rejected(BLK_ERR_INVALID);
    setup(); entry(0, 0x0c, 1, 32); image[446] = 1;
    check_rejected(BLK_ERR_INVALID);
    setup(); entry(0, 0, 1, 8); check_rejected(BLK_ERR_INVALID);
    setup(); entry(0, 0x83, 1, 32); check_rejected(BLK_ERR_UNSUPPORTED);
    setup(); entry(0, 0xee, 1, 127); check_rejected(BLK_ERR_UNSUPPORTED);
    setup(); entry(0, 0x0c, 1, 32); entry(1, 0x0f, 64, 32);
    check_rejected(BLK_ERR_UNSUPPORTED);
    setup(); read_error = BLK_ERR_IO; check_rejected(BLK_ERR_IO);
}
static void registry_limits(void) {
    setup();
    for (unsigned i = 0; i < 4; i++) entry(i, i == 3 ? 0x1b : 0x0c, 1 + i * 16, 8);
    CHECK(blkdev_register(NULL, BLKDEV_SD) == BLK_ERR_INVALID, "null backend rejected");
    CHECK(blkdev_register(&backend, (blkdev_type_t)99) == BLK_ERR_INVALID, "invalid type rejected");
    CHECK(blkdev_device_count() == 0, "invalid arguments do not consume slots");
    blkdev_t copies[BLKDEV_MAX_DEVICES];
    for (unsigned i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        copies[i] = backend;
        CHECK(blkdev_register(&copies[i], BLKDEV_USB) == 0, "register device %u", i);
    }
    CHECK(blkdev_device_count() == BLKDEV_MAX_DEVICES, "physical registry full");
    CHECK(blkdev_partition_count() == BLKDEV_MAX_PARTITIONS, "all partition slots usable");
    CHECK(blkdev_partition_get(BLKDEV_MAX_PARTITIONS - 1)->mbr_type == 0x1b,
          "hidden CHS FAT32 type retained");
    CHECK(blkdev_partition_io(BLKDEV_MAX_PARTITIONS) == NULL, "I/O lookup bounded");
    CHECK(blkdev_register(&copies[0], BLKDEV_USB) == BLK_ERR_BUSY, "duplicate rejected");
    CHECK(blkdev_register(&backend, BLKDEV_USB) == BLK_ERR_BUSY, "overflow rejected");
    blkdev_registry_reset();
    CHECK(blkdev_device_count() == 0 && blkdev_partition_count() == 0, "reset clears registry");
    CHECK(blk_read(0, 1, image) == BLK_ERR_NODEV, "reset clears legacy selection");
    CHECK(blkdev_root() == NULL, "reset clears root identity");
}
TEST_MAIN(bare_volume, mbr_and_io, malformed_media, registry_limits)
