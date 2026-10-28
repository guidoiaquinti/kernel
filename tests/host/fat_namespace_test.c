// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#include "fs/blkdev.h"
#include "fs/fat32.h"
#include "fs/namespace.h"
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
static unsigned fail_write_number, fail_flush_number, fail_allocation_number, allocation_calls;
static unsigned fail_write_after, fail_read_number, reads;
static uint8_t *source;
static size_t source_size;

static void copy_bytes(void *to, const void *from, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)to)[i] = ((const uint8_t *)from)[i];
}
static void fill(void *to, uint8_t value, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)to)[i] = value;
}
void *kmalloc(size_t n) {
    allocation_calls++;
    if (fail_allocation_number && allocation_calls == fail_allocation_number) return NULL;
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
    reads++;
    if (fail_read || (fail_read_number && reads == fail_read_number)) return BLK_ERR_IO;
    if ((uint64_t)lba + count > d->sector_count) return BLK_ERR_RANGE;
    copy_bytes(buf, (uint8_t *)d->ctx + (size_t)lba * 512, (size_t)count * 512);
    return 0;
}
static int memory_write(blkdev_t *d, uint32_t lba, uint32_t count, const void *buf) {
    writes++;
    if (fail_write || (fail_write_after && writes >= fail_write_after) || (fail_write_number && writes == fail_write_number) || (write_fail_lba >= 0 && lba <= (uint32_t)write_fail_lba &&
                      (uint64_t)lba + count > (uint32_t)write_fail_lba)) return BLK_ERR_IO;
    if ((uint64_t)lba + count > d->sector_count) return BLK_ERR_RANGE;
    copy_bytes((uint8_t *)d->ctx + (size_t)lba * 512, buf, (size_t)count * 512);
    return 0;
}
static int memory_flush(blkdev_t *d) {
    (void)d;
    flushes++;
    return fail_flush || (fail_flush_number && flushes == fail_flush_number) ? BLK_ERR_IO : 0;
}
static blkdev_t clone(void) {
    uint8_t *image = malloc(source_size);
    CHECK(image != NULL, "disposable image allocated");
    copy_bytes(image, source, source_size);
    fail_read = fail_write = fail_flush = false;
    write_fail_lba = -1;
    fail_write_number = fail_flush_number = fail_allocation_number = 0;
    allocation_calls = 0;
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
static unsigned length(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static void create_text(fat32_volume_t *v, const char *path, const char *text) {
    fat32_file_t file = {0};
    int rc = fat32_open_flags(v, path, FAT32_O_RDWR | FAT32_O_CREAT | FAT32_O_EXCL, &file);
    CHECK(rc == 0, "create new file %s: %d", path, rc);
    if (rc) return;
    CHECK(fat32_write(&file, text, length(text)) == (long)length(text), "write new file payload");
    fat32_close(&file);
}
static void check_text(fat32_volume_t *v, const char *path, const char *text) {
    fat32_file_t file = {0};
    uint8_t contents[1800];
    int rc = fat32_open(v, path, &file);
    CHECK(rc == 0, "open expected namespace file %s: %d", path, rc);
    if (rc) return;
    unsigned n = length(text);
    CHECK(fat32_size(&file) == n && fat32_read(&file, contents, n) == (long)n, "file size and read length");
    bool match = true;
    for (unsigned i = 0; i < n; i++) if (contents[i] != (uint8_t)text[i]) match = false;
    CHECK(match, "namespace file retains exact contents");
    fat32_close(&file);
}
static void long_path(char *out, unsigned characters, bool astral) {
    copy_bytes(out, "/a/", 3);
    unsigned pos = 3;
    const char *unit = astral ? "😀" : "中";
    unsigned bytes = astral ? 4 : 3;
    for (unsigned i = 0; i < characters; i++) {
        copy_bytes(out + pos, unit, bytes);
        pos += bytes;
    }
    if (astral) out[pos++] = 'a';
    out[pos] = 0;
}
static void alias_path(blkdev_t *dev, const fat32_dirent_t *entry, const char *parent, char *out) {
    const uint8_t *raw = (uint8_t *)dev->ctx + (size_t)entry->entry_sector * 512 + entry->entry_offset;
    unsigned pos = length(parent);
    copy_bytes(out, parent, pos);
    out[pos++] = '/';
    for (unsigned i = 0; i < 8 && raw[i] != ' '; i++) out[pos++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[pos++] = '.';
        for (unsigned i = 8; i < 11 && raw[i] != ' '; i++) out[pos++] = (char)raw[i];
    }
    out[pos] = 0;
}
static void normal_operations(void) {
    blkdev_t dev = clone();
    fat32_volume_t *volume = mount(&dev);
    check_text(volume, "/a/COLLIS~1.TXT", "owned original");
    create_text(volume, "/a/collision filename.txt", "new alias");
    CHECK(fat32_sync_volume(volume) == 0, "sync alias-shadow regression file");
    fat32_dirent_t shadow = {0};
    CHECK(fat32_lookup(volume, "/a/collision filename.txt", &shadow) == 0,
          "new alias-shadow long name resolves");
    char shadow_alias[64];
    alias_path(&dev, &shadow, "/a", shadow_alias);
    CHECK(strcmp(shadow_alias, "/a/COLLIS~1.TXT") != 0,
          "new raw short alias cannot collide with another entry's valid long name");
    check_text(volume, shadow_alias, "new alias");
    check_text(volume, "/a/COLLIS~1.TXT", "owned original");
    create_text(volume, "/a/plain.txt", "plain");
    create_text(volume, "/a/Δοκιμή.txt", "greek");
    create_text(volume, "/a/astral-😀.txt", "astral");
    char path[900];
    long_path(path, 255, false);
    create_text(volume, path, "max");
    long_path(path, 127, true);
    create_text(volume, path, "max astral");
    char aliases[12][64];
    for (unsigned i = 0; i < 12; i++) {
        snprintf(path, sizeof(path), "/a/collision filename number %u.txt", i);
        create_text(volume, path, "alias");
    }
    CHECK(fat32_sync_volume(volume) == 0, "sync long filename and short aliases");
    for (unsigned i = 0; i < 12; i++) {
        snprintf(path, sizeof(path), "/a/collision filename number %u.txt", i);
        fat32_dirent_t entry = {0};
        CHECK(fat32_lookup(volume, path, &entry) == 0, "lookup alias-collision entry");
        alias_path(&dev, &entry, "/a", aliases[i]);
        for (unsigned j = 0; j < i; j++) CHECK(strcmp(aliases[i], aliases[j]) != 0, "short aliases remain unique");
        check_text(volume, aliases[i], "alias");
        fat32_file_t duplicate = {0};
        CHECK(fat32_open_flags(volume, aliases[i], FAT32_O_RDWR | FAT32_O_CREAT | FAT32_O_EXCL,
                               &duplicate) == FS_ERR_EXISTS, "exclusive create rejects existing short alias");
        fat32_close(&duplicate);
    }
    fat32_file_t file = {0};
    CHECK(fat32_open_flags(volume, "/a/PLAIN.TXT", FAT32_O_CREAT | FAT32_O_EXCL | FAT32_O_RDWR,
                           &file) == FS_ERR_EXISTS, "exclusive create rejects case-insensitive duplicate");
    CHECK(fat32_open_flags(volume, "/a/plain.txt", FAT32_O_EXCL | FAT32_O_RDWR, &file) == FS_ERR_INVAL,
          "exclusive flag requires create");
    CHECK(fat32_open_flags(volume, "/a/plain.txt", FAT32_O_CREAT | FAT32_O_RDWR, &file) == 0,
          "ordinary create opens existing file without truncating");
    fat32_close(&file);
    check_text(volume, "/a/plain.txt", "plain");
    for (unsigned i = 0; i < 40; i++) {
        snprintf(path, sizeof(path), "/b/entry-%02u.txt", i);
        create_text(volume, path, "entry");
    }
    create_text(volume, "/a/temporary long filename deleted.txt", "delete slots");
    CHECK(fat32_unlink(volume, "/a/temporary long filename deleted.txt") == 0, "unlink releases LFN slots");
    create_text(volume, "/a/reused slots long filename.txt", "reuse");
    create_text(volume, "/a/case.txt", "case");
    CHECK(fat32_rename(volume, "/a/case.txt", "/a/CASE.TXT") == 0, "case-only rename succeeds");
    CHECK(fat32_rename(volume, "/a/source.bin", "/b/moved.bin") == 0, "move file across directories");
    CHECK(fat32_rename(volume, "/a/subdir", "/b/moved directory") == 0, "move directory and update dotdot");
    CHECK(fat32_unlink(volume, "/a/victim.bin") == 0, "unlink reclaims allocated file chain");
    CHECK(fat32_rmdir(volume, "/emptydir") == 0, "remove empty directory");
    CHECK(fat32_sync_volume(volume) == 0, "sync complete namespace mutations");
    CHECK(fat32_unmount(volume) == 0, "cold remount after namespace operations");
    volume = mount(&dev);
    check_text(volume, "/a/Δοκιμή.txt", "greek");
    check_text(volume, "/a/astral-😀.txt", "astral");
    long_path(path, 255, false);
    check_text(volume, path, "max");
    long_path(path, 127, true);
    check_text(volume, path, "max astral");
    check_text(volume, "/b/moved.bin", "source content");
    fat32_dirent_t entry = {0};
    CHECK(fat32_lookup(volume, "/a/victim.bin", &entry) == FS_ERR_NOTFOUND &&
          fat32_lookup(volume, "/a/source.bin", &entry) == FS_ERR_NOTFOUND &&
          fat32_lookup(volume, "/a/subdir", &entry) == FS_ERR_NOTFOUND, "deleted and renamed old paths disappear");
    save(&dev, "normal.img");
    finish(&dev, volume);
}
static void invalid_names(void) {
    blkdev_t dev = clone();
    fat32_volume_t *volume = mount(&dev);
    const char *bad[] = {"/a/overlong-\xc0\xaf", "/a/surrogate-\xed\xa0\x80",
                         "/a/range-\xf4\x90\x80\x80", "/a/truncated-\xe2\x82",
                         "/a/lone-\x80", "/a/padding-\xef\xbf\xbf", "/a/bad*name", "/a/bad?name", "/a/bad:name",
                         "/a/bad\\name", "/a/bad\"name", "/a/bad<name", "/a/bad>name",
                         "/a/bad|name", "/a/control-\x01", "/a/trailing.", "/a/trailing "};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        unsigned before = writes;
        fat32_file_t file = {0};
        CHECK(fat32_open_flags(volume, bad[i], FAT32_O_CREAT | FAT32_O_EXCL | FAT32_O_RDWR,
                               &file) < 0 && writes == before, "invalid UTF-8 or FAT component rejected before writing");
        fat32_close(&file);
    }
    char path[900];
    long_path(path, 256, false);
    fat32_file_t file = {0};
    CHECK(fat32_open_flags(volume, path, FAT32_O_CREAT | FAT32_O_RDWR, &file) < 0,
          "more than 255 UTF-16 units rejected");
    fat32_close(&file);
    long_path(path, 128, true);
    CHECK(fat32_open_flags(volume, path, FAT32_O_CREAT | FAT32_O_RDWR, &file) < 0,
          "astral surrogate pairs count as two UTF-16 units");
    fat32_close(&file);
    CHECK(fat32_open_flags(volume, "/a/new.txt", FAT32_O_CREAT | FAT32_O_RDONLY | FAT32_O_EXCL, &file) == 0,
          "read-only handle can create metadata on writable media");
    char empty;
    CHECK(fat32_read(&file, &empty, 1) == 0 && fat32_write(&file, "x", 1) == FS_ERR_RO,
          "created read-only handle permits reads and denies writes");
    fat32_close(&file);
    CHECK(fat32_open_flags(volume, "/a/new.txt", FAT32_O_CREAT | FAT32_O_RDONLY | FAT32_O_EXCL, &file) == FS_ERR_EXISTS,
          "read-only exclusive creation preserves existing entry");
    CHECK(fat32_open_flags(volume, "/a/new.txt", FAT32_O_RDONLY | FAT32_O_TRUNC, &file) == FS_ERR_INVAL &&
          fat32_open_flags(volume, "/a/new.txt", FAT32_O_RDONLY | FAT32_O_APPEND, &file) == FS_ERR_INVAL,
          "read-only truncate and append remain invalid");
    fat32_file_t root = {0};
    CHECK(fat32_opendir(volume, "/", &root) == 0, "open root for cursor overflow regression");
    root.pos = 0xffffffe0;
    fat32_dirent_t entry = {0};
    CHECK(fat32_readdir(&root, &entry) == FS_ERR_CORRUPT, "directory cursor overflow is rejected before wrapping");
    fat32_close(&root);
    CHECK(fat32_mkdir(volume, "/a/.") < 0 && fat32_rmdir(volume, "/") < 0,
          "dot and root cannot be namespace mutation targets");
    finish(&dev, volume);
}
static void guard_operations(void) {
    blkdev_t dev = clone();
    fat32_volume_t *volume = mount(&dev);
    fat32_file_t file = {0};
    CHECK(fat32_open(volume, "/a/source.bin", &file) == 0, "open file before busy mutation checks");
    CHECK(fat32_unlink(volume, "/a/source.bin") == FS_ERR_BUSY &&
          fat32_rename(volume, "/a/source.bin", "/b/moved.bin") == FS_ERR_BUSY,
          "open regular handles prevent unlink and rename");
    fat32_close(&file);
    CHECK(fat32_rename(volume, "/a/source.bin", "/a/victim.bin") == FS_ERR_EXISTS,
          "rename does not overwrite existing destination");
    create_text(volume, "/a/Mixed case long filename.txt", "case alias");
    CHECK(fat32_sync_volume(volume) == 0, "sync original case-only rename alias");
    fat32_dirent_t before_case = {0}, after_case = {0};
    char before_alias[64], after_alias[64];
    CHECK(fat32_lookup(volume, "/a/Mixed case long filename.txt", &before_case) == 0,
          "resolve original case-only long name");
    alias_path(&dev, &before_case, "/a", before_alias);
    CHECK(fat32_rename(volume, "/a/Mixed case long filename.txt", "/a/MIXED CASE LONG FILENAME.TXT") == 0 &&
          fat32_sync_volume(volume) == 0, "case-only long-name rename succeeds");
    CHECK(fat32_lookup(volume, "/a/MIXED CASE LONG FILENAME.TXT", &after_case) == 0,
          "resolve renamed case-only long name");
    alias_path(&dev, &after_case, "/a", after_alias);
    CHECK(strcmp(before_alias, after_alias) == 0, "case-only long-name rename preserves raw short alias");
    CHECK(fat32_rename(volume, "/a/MIXED CASE LONG FILENAME.TXT", after_alias) == FS_ERR_EXISTS,
          "rename cannot overwrite its own different short-alias naming form");
    check_text(volume, "/a/MIXED CASE LONG FILENAME.TXT", "case alias");
    CHECK(fat32_mkdir(volume, "/a/subdir") == FS_ERR_EXISTS, "mkdir rejects existing target");
    CHECK(fat32_rmdir(volume, "/a") == FS_ERR_NOTEMPTY, "rmdir rejects nonempty directory");
    CHECK(fat32_unlink(volume, "/a/subdir") < 0 && fat32_rmdir(volume, "/a/source.bin") < 0,
          "unlink and rmdir enforce object type");
    CHECK(fat32_rename(volume, "/a", "/a/subdir/cycle") < 0, "directory cannot move beneath itself");
    CHECK(fat32_opendir(volume, "/a/subdir", &file) == 0, "open directory handle");
    CHECK(fat32_rmdir(volume, "/a/subdir") == FS_ERR_BUSY &&
          fat32_rename(volume, "/a/subdir", "/b/moved") == FS_ERR_BUSY,
          "open directory handles prevent removal and relocation");
    fat32_close(&file);
    finish(&dev, volume);
    dev = clone();
    dev.read_only = true;
    volume = mount(&dev);
    CHECK(fat32_unlink(volume, "/a/source.bin") == FS_ERR_RO &&
          fat32_mkdir(volume, "/new") == FS_ERR_RO &&
          fat32_rmdir(volume, "/emptydir") == FS_ERR_RO &&
          fat32_rename(volume, "/a/source.bin", "/b/new") == FS_ERR_RO && writes == 0,
          "read-only namespace operations never change media");
    finish(&dev, volume);
}
static void qualified_namespace(void) {
    blkdev_t first = clone(), second = clone();
    blkdev_registry_reset();
    CHECK(blkdev_register(&first, BLKDEV_VIRTIO) == 0 &&
          blkdev_register(&second, BLKDEV_RAMDISK) == 0 &&
          fs_mount_registered(blkdev_partition_io(0)) == 0, "mount two writable namespace volumes");
    fs_cwd_t cwd = {0};
    CHECK(fs_boot_cwd(&cwd) == 0 && fs_chdir(&cwd, "df0:a") == 0, "pin caller current directory");
    fat32_file_t file = {0};
    CHECK(fs_open_flags(&cwd, "created while cwd pinned.txt", FAT32_O_CREAT | FAT32_O_RDWR, &file) == 0,
          "parent cwd pin allows creating a child file");
    CHECK(fat32_write(&file, "cwd", 3) == 3, "write file created relative to cwd");
    fat32_close(&file);
    CHECK(fs_mkdir(&cwd, "child") == 0 && fs_rmdir(&cwd, "child") == 0,
          "parent cwd pin allows adding and removing a child directory");
    CHECK(fs_unlink(&cwd, "created while cwd pinned.txt") == 0,
          "parent cwd pin permits removing an unpinned child file");
    CHECK(fs_rmdir(&cwd, "df0:a") == FS_ERR_BUSY && fs_rename(&cwd, "df0:a", "df0:renamed") == FS_ERR_BUSY,
          "cwd itself cannot be removed or moved");
    CHECK(fs_rename(&cwd, "source.bin", "df1:b/destination.bin") == FS_ERR_XDEV,
          "qualified rename rejects cross-volume destination");
    CHECK(fs_assign_set(&cwd, "held", "df0:a/subdir") == 0 &&
          fs_rmdir(&cwd, "subdir") == FS_ERR_BUSY, "assign target holds directory busy");
    CHECK(fs_assign_set(&cwd, "held", NULL) == 0 && fs_rmdir(&cwd, "subdir") == 0,
          "removing assign permits deleting empty target");
    CHECK(fs_rename(&cwd, "source.bin", "df0:b/qualified moved.bin") == 0,
          "relative source and qualified destination use same volume");
    fs_cwd_release(&cwd);
    CHECK(fs_namespace_reset() == 0, "release qualified namespace and sync both volumes");
    blkdev_registry_reset();
    free(first.ctx);
    free(second.ctx);
    CHECK(allocations == 0, "namespace wrapper cleanup retains no pins");
}
static const char *operations[] = {"create", "mkdir", "unlink", "rmdir", "rename", "dirrename"};
static int mutation(fat32_volume_t *volume, unsigned operation) {
    switch (operation) {
    case 0: {
        fat32_file_t file = {0};
        int rc = fat32_open_flags(volume, "/b/fault created xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.txt",
                                 FAT32_O_CREAT | FAT32_O_RDWR | FAT32_O_EXCL, &file);
        fat32_close(&file);
        return rc;
    }
    case 1: return fat32_mkdir(volume, "/b/fault directory xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    case 2: return fat32_unlink(volume, "/a/victim.bin");
    case 3: return fat32_rmdir(volume, "/emptydir");
    case 4: return fat32_rename(volume, "/a/source.bin", "/b/fault renamed xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.bin");
    default: return fat32_rename(volume, "/a/subdir", "/b/fault moved directory xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    }
}
static void check_prior_namespace(fat32_volume_t *volume, unsigned operation) {
    fat32_dirent_t entry = {0};
    switch (operation) {
    case 0: CHECK(fat32_lookup(volume, "/b/fault created xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.txt", &entry) == FS_ERR_NOTFOUND,
                  "failed create restores absent path"); break;
    case 1: CHECK(fat32_lookup(volume, "/b/fault directory xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", &entry) == FS_ERR_NOTFOUND,
                  "failed mkdir restores absent path"); break;
    case 2: CHECK(fat32_lookup(volume, "/a/victim.bin", &entry) == 0 && entry.size == 1300,
                  "failed unlink retains file metadata"); break;
    case 3: CHECK(fat32_lookup(volume, "/emptydir", &entry) == 0 && entry.is_dir,
                  "failed rmdir retains old directory"); break;
    case 4: check_text(volume, "/a/source.bin", "source content");
            CHECK(fat32_lookup(volume, "/b/fault renamed xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.bin", &entry) == FS_ERR_NOTFOUND,
                  "failed rename leaves destination absent"); break;
    default: CHECK(fat32_lookup(volume, "/a/subdir", &entry) == 0 && entry.is_dir,
                    "failed directory rename retains old path");
             CHECK(fat32_lookup(volume, "/b/fault moved directory xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", &entry) == FS_ERR_NOTFOUND,
                   "failed directory rename leaves destination absent"); break;
    }
}
static void interrupted_operations(void) {
    for (unsigned operation = 0; operation < 6; operation++) {
        for (unsigned kind = 0; kind < 2; kind++) {
            unsigned points = kind ? 4 : 8;
            for (unsigned point = 1; point <= points; point++) {
                blkdev_t dev = clone();
                fat32_volume_t *volume = mount(&dev);
                if (kind) fail_flush_number = point;
                else fail_write_number = point;
                int rc = mutation(volume, operation);
                int sync_rc = fat32_sync_volume(volume);
                fail_write_number = fail_flush_number = 0;
                CHECK(sync_rc == 0 || sync_rc == FS_ERR_IO, "namespace sync returns success or injected transport error");
                CHECK(fat32_sync_volume(volume) == 0, "retry sync recovers namespace mutation undo");
                if (rc < 0) {
                    CHECK(rc == FS_ERR_IO, "mutation fault propagates transport error");
                    check_prior_namespace(volume, operation);
                    CHECK(mutation(volume, operation) == 0, "namespace operation retries after rollback");
                }
                if (operation == 0) {
                    fat32_file_t file = {0};
                    CHECK(fat32_open_flags(volume, "/b/fault created xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.txt", FAT32_O_RDWR, &file) == 0,
                          "recovered create opens");
                    CHECK(fat32_write(&file, "created", 7) == 7, "recovered create receives file contents");
                    fat32_close(&file);
                }
                CHECK(fat32_sync_volume(volume) == 0, "sync final recovered namespace");
                CHECK(fat32_unmount(volume) == 0, "cold remount recovered namespace");
                volume = mount(&dev);
                if (operation == 0) check_text(volume, "/b/fault created xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.txt", "created");
                char name[80];
                snprintf(name, sizeof(name), "fault-%s-%s-%u.img", operations[operation], kind ? "flush" : "write", point);
                save(&dev, name);
                finish(&dev, volume);
            }
        }
    }
}
static uint32_t free_clusters(blkdev_t *dev) {
    uint32_t free_count = 0;
    for (uint32_t cluster = 2; cluster < cluster_count(dev) + 2; cluster++) {
        if ((fat_value(dev, 0, cluster) & 0x0fffffff) == 0) free_count++;
    }
    return free_count;
}
static void allocation_failures(void) {
    for (unsigned operation = 0; operation < 6; operation++) {
        blkdev_t baseline = clone();
        fat32_volume_t *volume = mount(&baseline);
        allocation_calls = 0;
        CHECK(mutation(volume, operation) == 0, "observe allocation count for successful namespace operation");
        unsigned points = allocation_calls;
        CHECK(points > 0 && points < 256, "namespace heap allocation count is bounded");
        finish(&baseline, volume);
        for (unsigned point = 1; point <= points; point++) {
            blkdev_t dev = clone();
            volume = mount(&dev);
            uint32_t before_free = free_clusters(&dev);
            unsigned before_allocations = allocations;
            allocation_calls = 0;
            fail_allocation_number = point;
            int rc = mutation(volume, operation);
            CHECK(allocation_calls >= point && rc < 0, "observed namespace allocation failure is reported");
            fail_allocation_number = 0;
            CHECK(fat32_sync_volume(volume) == 0, "namespace recovers after transient allocation failure");
            check_prior_namespace(volume, operation);
            CHECK(free_clusters(&dev) == before_free, "failed namespace allocation preserves free clusters");
            CHECK(allocations == before_allocations, "failed namespace allocation retains no undo/codec/inode objects");
            CHECK(mutation(volume, operation) == 0, "namespace mutation retries after allocation failure");
            if (operation == 0) {
                fat32_file_t file = {0};
                CHECK(fat32_open_flags(volume, "/b/fault created xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.txt", FAT32_O_RDWR, &file) == 0,
                      "allocation-recovered create opens");
                CHECK(fat32_write(&file, "created", 7) == 7, "allocation-recovered create receives data");
                fat32_close(&file);
            }
            CHECK(fat32_sync_volume(volume) == 0 && fat32_unmount(volume) == 0,
                  "cold remount allocation-recovered namespace");
            volume = mount(&dev);
            if (point == points) {
                char name[80];
                snprintf(name, sizeof(name), "fault-%s-allocation.img", operations[operation]);
                save(&dev, name);
            }
            finish(&dev, volume);
        }
    }
}
static void exhausted_directory_growth(void) {
    blkdev_t dev = clone();
    // BAD reservations intentionally force an exhausted FAT. This malformed
    // fault fixture is never exported as a filesystem-integrity result.
    for (uint32_t cluster = 2; cluster < cluster_count(&dev) + 2; cluster++) {
        if ((fat_value(&dev, 0, cluster) & 0x0fffffff) != 0) continue;
        set_fat(&dev, 0, cluster, 0x0ffffff7);
        set_fat(&dev, 1, cluster, 0x0ffffff7);
    }
    fat32_volume_t *volume = mount(&dev);
    char path[900];
    long_path(path, 255, false);
    fat32_file_t file = {0};
    CHECK(fat32_open_flags(volume, path, FAT32_O_CREAT | FAT32_O_RDWR, &file) == FS_ERR_NOSPC,
          "directory extension for long name reports exhausted clusters");
    fat32_close(&file);
    CHECK(fat32_mkdir(volume, "/b/full") == FS_ERR_NOSPC, "new directory needs a free cluster");
    CHECK(fat32_open_flags(volume, "/b/empty.txt", FAT32_O_CREAT | FAT32_O_RDWR, &file) == 0,
          "clusterless empty file can use existing directory slots on full media");
    fat32_close(&file);
    CHECK(fat32_unlink(volume, "/a/source.bin") == 0 && fat32_mkdir(volume, "/b/reused") == 0,
          "unlink releases a cluster for subsequent directory creation");
    finish(&dev, volume);
}
static void retained_rollback(void) {
    const unsigned tested[] = {0, 1, 2, 3};
    for (unsigned i = 0; i < sizeof(tested) / sizeof(tested[0]); i++) {
        unsigned operation = tested[i];
        blkdev_t dev = clone();
        fat32_volume_t *volume = mount(&dev);
        uint32_t before_free = free_clusters(&dev);
        fail_write_after = 3;
        int rc = mutation(volume, operation);
        CHECK(rc == 0 || rc == FS_ERR_IO, "persistent fault preserves mutation return contract");
        CHECK(fat32_sync_volume(volume) == FS_ERR_IO, "failed undo write remains pending for later retry");
        CHECK(fat32_unmount(volume) == FS_ERR_IO, "failed undo prevents volume teardown");
        fail_write_after = 0;
        CHECK(fat32_sync_volume(volume) == 0, "retained rollback completes when transport recovers");
        if (rc < 0) {
            check_prior_namespace(volume, operation);
            CHECK(free_clusters(&dev) == before_free, "persistent rollback restores prior free clusters");
            CHECK(mutation(volume, operation) == 0, "namespace mutation retries after persistent rollback");
        } else {
            fat32_dirent_t entry = {0};
            const char *old_path = operation == 2 ? "/a/victim.bin" : "/emptydir";
            CHECK(fat32_lookup(volume, old_path, &entry) == FS_ERR_NOTFOUND,
                  "successful deletion survives failed sync and later retry");
        }
        if (operation == 0) {
            fat32_file_t file = {0};
            CHECK(fat32_open_flags(volume, "/b/fault created xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.txt", FAT32_O_RDWR, &file) == 0,
                  "persistent-recovered create opens");
            CHECK(fat32_write(&file, "created", 7) == 7, "persistent-recovered create data");
            fat32_close(&file);
        }
        CHECK(fat32_sync_volume(volume) == 0, "sync persistent-recovered namespace");
        char name[80];
        snprintf(name, sizeof(name), "fault-%s-persistent.img", operations[operation]);
        save(&dev, name);
        finish(&dev, volume);
    }
}
static void namespace_read_failures(void) {
    const unsigned tested[] = {1, 3, 8, 16};
    for (unsigned operation = 0; operation < 6; operation++) {
        for (unsigned i = 0; i < sizeof(tested) / sizeof(tested[0]); i++) {
            blkdev_t dev = clone();
            fat32_volume_t *volume = mount(&dev);
            uint32_t before_free = free_clusters(&dev);
            reads = 0;
            fail_read_number = tested[i];
            int rc = mutation(volume, operation);
            bool reached = reads >= tested[i];
            fail_read_number = 0;
            CHECK(!reached || rc == FS_ERR_IO, "namespace reports injected required-sector read failure");
            CHECK(fat32_sync_volume(volume) == 0, "namespace recovers after required-sector read error");
            if (rc < 0) {
                check_prior_namespace(volume, operation);
                CHECK(free_clusters(&dev) == before_free, "read-failed namespace mutation preserves allocation state");
                CHECK(mutation(volume, operation) == 0, "retry read-failed namespace mutation");
            }
            finish(&dev, volume);
        }
    }
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    directory = argv[1];
    char path[1024];
    snprintf(path, sizeof(path), "%s/source.img", directory);
    void *stream = fopen(path, "rb");
    if (!stream) return 2;
    fseek(stream, 0, 2);
    source_size = (size_t)ftell(stream);
    fseek(stream, 0, 0);
    source = malloc(source_size);
    CHECK(source && fread(source, 1, source_size, stream) == source_size, "load immutable namespace source");
    fclose(stream);
    normal_operations();
    invalid_names();
    guard_operations();
    qualified_namespace();
    interrupted_operations();
    allocation_failures();
    exhausted_directory_growth();
    retained_rollback();
    namespace_read_failures();
    free(source);
    printf("fat_namespace_test: %d checks, %d failures\n", test_checks, test_failures);
    return test_failures != 0;
}
