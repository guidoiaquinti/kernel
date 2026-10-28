// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#include "fs/blkdev.h"
#include "fs/fat32.h"
#include "mm/kmalloc.h"

void *fopen(const char *, const char *);
int fclose(void *);
int fseek(void *, long, int);
long ftell(void *);
size_t fread(void *, size_t, size_t, void *);
size_t fwrite(const void *, size_t, size_t, void *);
void *malloc(size_t);
void free(void *);
int posix_memalign(void **, size_t, size_t);
int snprintf(char *, size_t, const char *, ...);
int test_failures, test_checks;
static const char *directory;
static unsigned allocations, writes, flushes;
static bool fail_read, fail_write, fail_flush;
static int write_fail_lba = -1;
static unsigned fail_write_number;
static uint8_t *source;
static size_t source_size;

static void copy_bytes(void *to, const void *from, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)to)[i] = ((const uint8_t *)from)[i];
}
static void fill(void *to, uint8_t value, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)to)[i] = value;
}
void *kmalloc(size_t n) {
    void *p = NULL;
    if (posix_memalign(&p, 64, n)) return NULL;
    fill(p, 0, n);
    allocations++;
    return p;
}
void kfree(void *p) {
    if (!p) return;
    CHECK(allocations > 0, "balanced allocator releases");
    allocations--;
    free(p);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}
static uint32_t fat_start(blkdev_t *d) { return get16((uint8_t *)d->ctx + 14); }
static uint32_t fat_size(blkdev_t *d) { return get32((uint8_t *)d->ctx + 36); }
static uint32_t cluster_count(blkdev_t *d) {
    uint8_t *r = d->ctx;
    return (get32(r + 32) - fat_start(d) - r[16] * fat_size(d)) / r[13];
}
static uint32_t fat_value(blkdev_t *d, unsigned f, uint32_t c) {
    return get32((uint8_t *)d->ctx + (fat_start(d) + f * fat_size(d)) * 512 + c * 4);
}
static void set_fat(blkdev_t *d, unsigned f, uint32_t c, uint32_t value) {
    put32((uint8_t *)d->ctx + (fat_start(d) + f * fat_size(d)) * 512 + c * 4, value);
}
static int memory_read(blkdev_t *d, uint32_t lba, uint32_t count, void *buf) {
    if (fail_read) return BLK_ERR_IO;
    if ((uint64_t)lba + count > d->sector_count) return BLK_ERR_RANGE;
    copy_bytes(buf, (uint8_t *)d->ctx + (size_t)lba * 512, (size_t)count * 512);
    return 0;
}
static int memory_write(blkdev_t *d, uint32_t lba, uint32_t count, const void *buf) {
    writes++;
    if (fail_write || (fail_write_number && writes == fail_write_number) || (write_fail_lba >= 0 && lba <= (uint32_t)write_fail_lba &&
                      (uint64_t)lba + count > (uint32_t)write_fail_lba)) return BLK_ERR_IO;
    if ((uint64_t)lba + count > d->sector_count) return BLK_ERR_RANGE;
    copy_bytes((uint8_t *)d->ctx + (size_t)lba * 512, buf, (size_t)count * 512);
    return 0;
}
static int memory_flush(blkdev_t *d) {
    (void)d;
    flushes++;
    return fail_flush ? BLK_ERR_IO : 0;
}
static blkdev_t clone(void) {
    uint8_t *image = malloc(source_size);
    CHECK(image != NULL, "disposable image allocated");
    copy_bytes(image, source, source_size);
    fail_read = fail_write = fail_flush = false;
    write_fail_lba = -1;
    fail_write_number = 0;
    writes = flushes = 0;
    return (blkdev_t){.read = memory_read, .write = memory_write, .flush = memory_flush,
                     .sector_count = (uint32_t)(source_size / 512), .ctx = image};
}
static fat32_volume_t *mount(blkdev_t *d) {
    fat32_volume_t *v = NULL;
    CHECK(fat32_mount(d, &v) == 0 && v != NULL, "valid fixture mounts");
    return v;
}
static void finish(blkdev_t *d, fat32_volume_t *v) {
    CHECK(fat32_unmount(v) == 0, "closed synced volume unmounts");
    free(d->ctx);
    CHECK(allocations == 0, "all filesystem objects released");
}
static void save(blkdev_t *d, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    void *s = fopen(path, "wb");
    CHECK(s != NULL, "export disposable image");
    if (!s) return;
    CHECK(fwrite(d->ctx, 1, source_size, s) == source_size, "whole image exported");
    fclose(s);
}
static void read_uniform(fat32_file_t *f, uint8_t expected, uint32_t n) {
    uint8_t b[18000];
    CHECK(fat32_seek(f, 0, 0) == 0, "rewind handle");
    CHECK(fat32_read(f, b, n) == (long)n, "read expected length");
    bool matches = true;
    for (uint32_t i = 0; i < n; i++) if (b[i] != expected) matches = false;
    CHECK(matches, "all bytes survive cache pressure and remount");
}
static void integrity(const char *output, unsigned info_mode) {
    blkdev_t d = clone();
    uint8_t *raw = d.ctx;
    if (info_mode) {
        unsigned info = get16(raw + 48);
        put32(raw + info * 512 + 488, info_mode == 1 ? 0xffffffff : 0);
        put32(raw + info * 512 + 492, info_mode == 1 ? 0xffffffff : 0xfffffffe);
    }
    fat32_volume_t *v = mount(&d);
    fat32_file_t f = {0}, second = {0};
    CHECK(fat32_open_flags(v, "/original.bin", FAT32_O_RDWR, &f) == 0, "open existing data for writing");
    CHECK(fat32_open(v, "/original.bin", &second) == 0, "second handle shares metadata");
    CHECK(fat32_unmount(v) == FS_ERR_BUSY, "live handles prevent unmount");
    CHECK(fat32_seek(&f, 17, 0) == 17, "seek within file");
    CHECK(fat32_write(&f, "partial-sector", 14) == 14, "partial-sector write");
    uint8_t b[18000];
    CHECK(fat32_read(&second, b, 700) == 700, "second handle reads at independent cursor");
    bool preserved = true;
    for (unsigned i = 0; i < 700; i++) {
        uint8_t expected = i >= 17 && i < 31 ? (uint8_t)"partial-sector"[i - 17] : (uint8_t)(i * 37 + 11);
        if (b[i] != expected) preserved = false;
    }
    CHECK(preserved, "unwritten sector bytes preserved");
    CHECK(f.pos == 31 && second.pos == 700, "offsets remain independent");
    CHECK(fat32_write(&second, "x", 1) == FS_ERR_RO, "read-only handle rejects write");
    fat32_close(&f);
    CHECK(fat32_open_flags(v, "/original.bin", FAT32_O_WRONLY | FAT32_O_APPEND, &f) == 0, "append open");
    CHECK(fat32_seek(&f, 0, 0) == 0, "append seek changes cursor");
    fill(b, 'A', 1400);
    CHECK(fat32_write(&f, b, 1400) == 1400, "append extends across clusters");
    CHECK(fat32_size(&second) == 2100 && second.pos == 700, "size shared, offsets independent");
    CHECK(fat32_read(&second, b, 1400) == 1400, "old reader sees new tail");
    CHECK(fat32_read(&f, b, 1) == FS_ERR_INVAL, "write-only handle rejects read");
    CHECK(fat32_seek(&f, 2101, 0) < 0, "seek cannot create holes");
    CHECK(fat32_seek(&f, -4294967295L, 99) == FS_ERR_INVAL, "invalid seek whence rejected before wide offset arithmetic");
    CHECK(fat32_seek(&f, 4294967295L, 2) == FS_ERR_INVAL, "large positive EOF offset cannot overflow");
    CHECK(fat32_seek(&f, -1, 2) == 2099, "valid negative EOF offset");
    fat32_close(&second);
    fat32_close(&f);
    CHECK(fat32_open_flags(v, "/empty.bin", FAT32_O_RDWR, &f) == 0, "open clusterless file");
    CHECK(fat32_write(&f, NULL, 0) == 0, "zero-byte write needs no buffer");
    fill(b, 'E', 1700);
    CHECK(fat32_write(&f, b, 1700) == 1700, "allocate first cluster and tail");
    fat32_close(&f);
    CHECK(fat32_open_flags(v, "/fragment.bin", FAT32_O_RDWR, &f) == 0, "open fragmented chain");
    CHECK((fat_value(&d, 0, f.first_cluster) & 0x0fffffff) != f.first_cluster + 1, "fixture has genuine non-contiguous cluster chain");
    CHECK(fat32_truncate(&f, 777) == 0 && fat32_size(&f) == 777, "shrink chain");
    CHECK(fat32_truncate(&f, 778) < 0, "truncate cannot grow");
    fat32_close(&f);
    CHECK(fat32_open_flags(v, "/pressure.bin", FAT32_O_RDWR | FAT32_O_TRUNC, &f) == 0, "truncate-on-open frees chain");
    CHECK(fat32_size(&f) == 0, "truncate publishes empty metadata");
    fill(b, 'P', 16384);
    CHECK(fat32_write(&f, b, 16384) == 16384, "write exceeds sector cache capacity");
    read_uniform(&f, 'P', 16384);
    fat32_close(&f);
    CHECK(fat32_open_flags(v, "/reuse.bin", FAT32_O_RDWR, &f) == 0, "open file for further allocations");
    fill(b, 'R', 2200);
    CHECK(fat32_write(&f, b, 2200) == 2200, "allocate after freeing chains");
    fat32_close(&f);
    CHECK(fat32_sync_volume(v) == 0 && flushes > 0, "sync requests durable flush");
    CHECK(fat32_unmount(v) == 0, "unmount before cold remount");
    v = mount(&d);
    CHECK(fat32_open(v, "/empty.bin", &f) == 0 && fat32_size(&f) == 1700, "remount finds updated metadata");
    read_uniform(&f, 'E', 1700);
    fat32_close(&f);
    save(&d, output);
    finish(&d, v);
}
static void readonly_and_invalid(void) {
    blkdev_t d = clone();
    d.read_only = true;
    fat32_volume_t *v = mount(&d);
    fat32_file_t f = {0};
    CHECK(fat32_open_flags(v, "/original.bin", FAT32_O_RDWR, &f) == FS_ERR_RO, "read-only medium rejects write open");
    CHECK(fat32_open(v, "/original.bin", &f) == 0, "read-only medium remains readable");
    CHECK(fat32_truncate(&f, 0) == FS_ERR_RO, "read-only truncate rejected");
    CHECK(fat32_write(&f, "x", 1) == FS_ERR_RO && writes == 0, "read-only handle performs no write I/O");
    fat32_close(&f);
    CHECK(fat32_open_flags(v, "/missing", FAT32_O_CREAT | FAT32_O_WRONLY, &f) < 0, "read-only creation rejected");
    CHECK(fat32_open_flags(v, "/original.bin", 3, &f) == FS_ERR_INVAL, "invalid access flags rejected");
    finish(&d, v);
}
static void disk_full_and_high_bits(void) {
    blkdev_t d = clone();
    unsigned remaining = 2;
    uint32_t chosen[2] = {0};
    for (uint32_t c = 2; c < cluster_count(&d) + 2; c++) {
        if ((fat_value(&d, 0, c) & 0x0fffffff) != 0) continue;
        uint32_t value = 0x0ffffff7;
        if (remaining) {
            chosen[2 - remaining--] = c;
            value = 0xa0000000;
        }
        set_fat(&d, 0, c, value);
        set_fat(&d, 1, c, value);
    }
    // Deliberately malformed BAD reservations bound the NOSPC test. This image
    // is not exported and has no filesystem-integrity claim.
    fat32_volume_t *v = mount(&d);
    fat32_file_t f = {0};
    CHECK(fat32_open_flags(v, "/empty.bin", FAT32_O_RDWR, &f) == 0, "open full-disk test file");
    uint8_t b[1536];
    fill(b, 'D', sizeof(b));
    CHECK(fat32_write(&f, b, sizeof(b)) == 1024, "disk full returns completed short write");
    CHECK(fat32_size(&f) == 1024 && f.pos == 1024, "short-write size and offset match progress");
    CHECK(fat32_write(&f, b, 1) == FS_ERR_NOSPC, "disk full before progress is NOSPC");
    CHECK(fat32_sync_volume(v) == 0, "full-disk state syncs");
    CHECK((fat_value(&d, 0, chosen[0]) & 0xf0000000) == 0xa0000000 &&
          fat_value(&d, 0, chosen[0]) == fat_value(&d, 1, chosen[0]), "allocation preserves FAT high bits and mirrors");
    CHECK(fat32_truncate(&f, 0) == 0, "truncate zero frees allocated clusters");
    CHECK(fat32_seek(&f, 0, 0) == 0, "rewind independent cursor after truncate");
    CHECK(fat32_write(&f, b, 1024) == 1024, "freed clusters reused after full scan");
    CHECK(fat32_sync_volume(v) == 0, "reused chain syncs");
    fat32_close(&f);
    finish(&d, v);
}
static void active_fat(void) {
    blkdev_t d = clone();
    uint8_t *raw = d.ctx;
    raw[40] = 0x81;
    raw[41] = 0;
    size_t n = (size_t)fat_size(&d) * 512;
    uint8_t *inactive = malloc(n);
    copy_bytes(inactive, raw + fat_start(&d) * 512, n);
    fat32_volume_t *v = mount(&d);
    fat32_file_t f = {0};
    CHECK(fat32_open_flags(v, "/empty.bin", FAT32_O_RDWR, &f) == 0, "open with active second FAT");
    uint8_t b[1200];
    fill(b, 'F', sizeof(b));
    CHECK(fat32_write(&f, b, sizeof(b)) == (long)sizeof(b), "allocate via selected FAT");
    CHECK(fat32_sync_volume(v) == 0, "active FAT sync");
    bool unchanged = true;
    for (size_t i = 0; i < n; i++) if (inactive[i] != raw[fat_start(&d) * 512 + i]) unchanged = false;
    CHECK(unchanged, "inactive FAT remains untouched when mirroring disabled");
    fat32_close(&f);
    free(inactive);
    finish(&d, v);
}
static void corrupt_truncate(void) {
    blkdev_t d = clone();
    fat32_volume_t *v = mount(&d);
    fat32_dirent_t victim, unrelated;
    CHECK(fat32_lookup(v, "/fragment.bin", &victim) == 0 &&
          fat32_lookup(v, "/blocker.bin", &unrelated) == 0, "resolve corruption test chains");
    CHECK(fat32_unmount(v) == 0, "discard caches before corrupting media");
    uint32_t retained = fat_value(&d, 0, unrelated.first_cluster);
    set_fat(&d, 0, victim.first_cluster, victim.first_cluster);
    set_fat(&d, 1, victim.first_cluster, victim.first_cluster);
    v = mount(&d);
    fat32_file_t f = {0};
    CHECK(fat32_open_flags(v, "/fragment.bin", FAT32_O_RDWR, &f) == 0, "looping chain entry opens");
    CHECK(fat32_truncate(&f, 0) == FS_ERR_CORRUPT, "validate chain before freeing");
    CHECK(fat32_size(&f) == 3000 && fat_value(&d, 0, unrelated.first_cluster) == retained,
          "corrupt truncate preserves metadata and unrelated cluster");
    fat32_close(&f);
    finish(&d, v);
}
static void io_failures(void) {
    for (unsigned mode = 0; mode < 4; mode++) {
        blkdev_t d = clone();
        fat32_volume_t *v = mount(&d);
        fat32_file_t f = {0};
        CHECK(fat32_open_flags(v, "/pressure.bin", FAT32_O_RDWR, &f) == 0, "open failure test file");
        uint8_t b[16384];
        fill(b, 'I', sizeof(b));
        if (mode == 0) {
            fail_read = true;
            CHECK(fat32_write(&f, b, 1) == FS_ERR_IO, "write propagates required read error");
            fail_read = false;
            CHECK(f.pos == 0, "failed first byte leaves offset");
        } else if (mode == 1) {
            CHECK(fat32_write(&f, b, 1) == 1, "persist dirty marker before eviction fault");
            fail_write = true;
            long progress = fat32_write(&f, b + 1, sizeof(b) - 1);
            CHECK(progress > 0 && progress < (long)sizeof(b) - 1, "dirty-eviction error returns completed short write");
            CHECK(fat32_sync_volume(v) == FS_ERR_IO, "sync reports retained dirty error");
            fail_write = false;
            CHECK(fat32_sync_volume(v) == 0, "retry flush preserves dirty data without rewriting it");
            uint32_t completed = f.pos;
            read_uniform(&f, 'I', completed);
            CHECK(fat32_seek(&f, completed, 0) == (long)completed, "resume at short-write boundary");
            CHECK(fat32_write(&f, b + completed, sizeof(b) - completed) == (long)(sizeof(b) - completed), "complete only unwritten suffix");
        } else if (mode == 2) {
            write_fail_lba = (int)(fat_start(&d) + fat_size(&d));
            long rc = fat32_write(&f, b, 1);
            CHECK(rc == 1 || rc == FS_ERR_IO, "mirror failure preserves write return contract");
            CHECK(fat32_sync_volume(v) == FS_ERR_IO, "mirror write error reaches sync caller");
            write_fail_lba = -1;
        } else {
            CHECK(fat32_write(&f, b, 1) == 1, "mutate before flush failure");
            fail_flush = true;
            CHECK(fat32_sync_volume(v) == FS_ERR_IO, "flush failure reaches caller");
            CHECK((fat_value(&d, 0, 1) & 0x08000000) == 0, "failed flush keeps FAT dirty");
            fail_flush = false;
        }
        CHECK(fat32_seek(&f, 0, 0) == 0, "rewind after I/O recovery");
        CHECK(fat32_write(&f, b, sizeof(b)) == (long)sizeof(b), "retry transient error");
        CHECK(fat32_sync_volume(v) == 0, "retained cache is retryable");
        fat32_close(&f);
        CHECK(fat32_unmount(v) == 0, "recovered volume unmounts");
        v = mount(&d);
        CHECK(fat32_open(v, "/pressure.bin", &f) == 0, "recovered file remounts");
        read_uniform(&f, 'I', sizeof(b));
        fat32_close(&f);
        finish(&d, v);
    }
}
static void interrupted_metadata(void) {
    // Exercise failures at different transport writes, including initial dirty
    // marking, zero-before-link allocation, FAT mirrors and truncate reclaim.
    for (unsigned operation = 0; operation < 2; operation++) {
        for (unsigned point = 1; point <= 12; point++) {
            blkdev_t d = clone();
            fat32_volume_t *v = mount(&d);
            fat32_file_t f = {0};
            CHECK(fat32_open_flags(v, operation ? "/pressure.bin" : "/original.bin",
                                  FAT32_O_RDWR, &f) == 0, "open interrupted-metadata file");
            uint8_t b[18000];
            fill(b, 'G', 600);
            if (!operation) CHECK(fat32_seek(&f, 0, 2) == 700, "append location before allocation fault");
            fail_write_number = point;
            long rc = operation ? fat32_truncate(&f, 777) : fat32_write(&f, b, 600);
            bool triggered = writes >= point;
            if (triggered) CHECK(rc < (operation ? 0 : 600), "injected mutation failure is reported");
            fail_write_number = 0;
            CHECK(fat32_sync_volume(v) == 0, "recover interrupted FAT and detached-tail state");
            if (operation) {
                CHECK(fat32_truncate(&f, 777) == 0, "retry shrink after recovery");
            } else {
                uint32_t remaining = 1300 - fat32_size(&f);
                CHECK(fat32_seek(&f, 0, 2) == (long)fat32_size(&f), "seek recovered shared EOF");
                CHECK(fat32_write(&f, b, remaining) == (long)remaining, "retry only incomplete allocation bytes");
            }
            CHECK(fat32_sync_volume(v) == 0, "recovered metadata sync");
            fat32_close(&f);
            CHECK(fat32_unmount(v) == 0, "cold remount interrupted operation");
            v = mount(&d);
            CHECK(fat32_open(v, operation ? "/pressure.bin" : "/original.bin", &f) == 0,
                  "recovered short entry opens after remount");
            uint32_t size = operation ? 777 : 1300;
            CHECK(fat32_size(&f) == size && fat32_read(&f, b, size) == (long)size,
                  "recovered file has complete expected length");
            bool match = true;
            for (uint32_t i = 0; i < size; i++) {
                uint8_t expected = !operation && i >= 700 ? 'G' : (uint8_t)(i * 37 + 11);
                if (b[i] != expected) match = false;
            }
            CHECK(match, "interrupted operation preserves exact data");
            fat32_close(&f);
            char name[64];
            snprintf(name, sizeof(name), "fault-%s-%u.img", operation ? "shrink" : "grow", point);
            save(&d, name);
            finish(&d, v);
        }
    }
}
static int boot_only_read(blkdev_t *d, uint32_t lba, uint32_t count, void *buf) {
    if (lba != 0 || count != 1) return BLK_ERR_IO;
    copy_bytes(buf, d->ctx, 512);
    return 0;
}
static void invalid_reserved_geometry(void) {
    uint8_t boot[512];
    copy_bytes(boot, source, sizeof(boot));
    boot[13] = 1;
    boot[16] = 1;
    put32(boot + 36, 0x200000);
    uint32_t total = get16(boot + 14) + 0x200000 + 0x0fffffef;
    put32(boot + 32, total);
    blkdev_t d = {.read = boot_only_read, .read_only = true, .sector_count = total, .ctx = boot};
    fat32_volume_t *v = NULL;
    CHECK(fat32_mount(&d, &v) == FS_ERR_NOFS && v == NULL,
          "geometry cannot admit FAT reserved-marker data cluster numbers");
    CHECK(allocations == 0, "oversized geometry rejected before allocation");
}
static void unsupported_flush(void) {
    blkdev_t d = clone();
    d.flush = NULL;
    fat32_volume_t *v = mount(&d);
    fat32_file_t f = {0};
    CHECK(fat32_open_flags(v, "/original.bin", FAT32_O_RDWR, &f) == FS_ERR_UNSUPPORTED && writes == 0,
          "missing flush support rejects writable open before media write");
    fat32_close(&f);
    finish(&d, v);
}
static void partition_capabilities(void) {
    for (unsigned missing = 0; missing < 2; missing++) {
        blkdev_t backend = clone();
        if (missing == 0) backend.write = NULL;
        else backend.flush = NULL;
        blkdev_registry_reset();
        CHECK(blkdev_register(&backend, BLKDEV_VIRTIO) == 0 &&
              blkdev_partition_count() == 1, "register backend with incomplete writable capabilities");
        blkdev_t *view = blkdev_partition_io(0);
        CHECK(view && !view->read_only, "partition view retains negotiated read-only state");
        fat32_volume_t *volume = mount(view);
        fat32_file_t file = {0};
        CHECK(fat32_open_flags(volume, "/original.bin", FAT32_O_RDWR, &file) == FS_ERR_UNSUPPORTED &&
              writes == 0, "partition view rejects missing write or flush before media mutation");
        CHECK(fat32_sync_volume(volume) == 0, "unsupported write open leaves volume clean");
        fat32_close(&file);
        CHECK(fat32_unmount(volume) == 0, "unsupported-capability volume remains healthy for unmount");
        blkdev_registry_reset();
        free(backend.ctx);
        CHECK(allocations == 0, "partition capability checks release all allocations");
    }
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    directory = argv[1];
    char path[1024];
    snprintf(path, sizeof(path), "%s/source.img", directory);
    void *s = fopen(path, "rb");
    if (!s) return 2;
    fseek(s, 0, 2);
    source_size = (size_t)ftell(s);
    fseek(s, 0, 0);
    source = malloc(source_size);
    CHECK(source && fread(source, 1, source_size, s) == source_size, "load immutable source");
    fclose(s);
    integrity("written.img", 0);
    integrity("unknown-info.img", 1);
    integrity("stale-info.img", 2);
    readonly_and_invalid();
    disk_full_and_high_bits();
    active_fat();
    corrupt_truncate();
    io_failures();
    interrupted_metadata();
    invalid_reserved_geometry();
    unsupported_flush();
    partition_capabilities();
    free(source);
    printf("fat_write_test: %d checks, %d failures\n", test_checks, test_failures);
    return test_failures != 0;
}
