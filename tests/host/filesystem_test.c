// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#include "fs/blkdev.h"
#include "fs/fat32.h"
#include "fs/namespace.h"
#include "mm/kmalloc.h"

void *fopen(const char *path, const char *mode);
int fclose(void *stream);
int fseek(void *stream, long offset, int whence);
long ftell(void *stream);
size_t fread(void *ptr, size_t size, size_t count, void *stream);
void *malloc(size_t size);
void free(void *ptr);
int posix_memalign(void **out, size_t align, size_t size);
int snprintf(char *buf, size_t size, const char *format, ...);

const unsigned char _koraos_fs_start[512] = {0};
const unsigned char _koraos_fs_end[1] = {0};
int test_failures, test_checks;
static const char *fixture_directory;
static unsigned live_allocations;
static bool fail_allocations;
static int backend_error;
static unsigned backend_reads, backend_read_limit;
static int backend_flush_error;
static blkdev_t *failing_sync_backend;
static blkdev_t *observed_sync_backend;
static unsigned observed_sync_flushes;

void *kmalloc(size_t size) {
    if (fail_allocations) return NULL;
    void *result = NULL;
    if (posix_memalign(&result, 64, size) != 0) return NULL;
    uint8_t *p = result;
    for (size_t i = 0; i < size; i++) p[i] = 0;
    live_allocations++;
    return result;
}
void kfree(void *pointer) {
    if (!pointer) return;
    CHECK(live_allocations > 0, "allocator release balanced");
    live_allocations--;
    free(pointer);
}
static int memory_read(blkdev_t *dev, uint32_t lba, uint32_t count, void *buf) {
    if (backend_error) return backend_error;
    if (backend_read_limit && ++backend_reads > backend_read_limit) return BLK_ERR_IO;
    if ((uint64_t)lba + count > dev->sector_count) return BLK_ERR_RANGE;
    const uint8_t *source = (const uint8_t *)dev->ctx + (size_t)lba * BLK_SECTOR_SIZE;
    uint8_t *destination = buf;
    for (size_t i = 0; i < (size_t)count * BLK_SECTOR_SIZE; i++) destination[i] = source[i];
    return 0;
}
static int memory_write(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf) {
    if ((uint64_t)lba + count > dev->sector_count) return BLK_ERR_RANGE;
    uint8_t *destination = (uint8_t *)dev->ctx + (size_t)lba * BLK_SECTOR_SIZE;
    const uint8_t *source = buf;
    for (size_t i = 0; i < (size_t)count * BLK_SECTOR_SIZE; i++) destination[i] = source[i];
    return 0;
}
static int memory_flush(blkdev_t *dev) {
    (void)dev;
    return backend_flush_error;
}
static int selective_flush(blkdev_t *dev) {
    if (dev == failing_sync_backend) return BLK_ERR_IO;
    if (dev == observed_sync_backend) observed_sync_flushes++;
    return 0;
}
static bool load_backend(const char *name, blkdev_t *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", fixture_directory, name);
    void *stream = fopen(path, "rb");
    CHECK(stream != NULL, "fixture available: %s", name);
    if (!stream) return false;
    fseek(stream, 0, 2);
    long size = ftell(stream);
    fseek(stream, 0, 0);
    void *image = malloc((size_t)size);
    CHECK(image != NULL && size > 0 && size % BLK_SECTOR_SIZE == 0, "whole-sector fixture");
    if (!image || size <= 0 || size % BLK_SECTOR_SIZE) {
        free(image);
        fclose(stream);
        return false;
    }
    CHECK(fread(image, 1, (size_t)size, stream) == (size_t)size, "complete fixture read");
    fclose(stream);
    *out = (blkdev_t){.read = memory_read, .read_only = true,
                     .sector_count = (uint32_t)(size / BLK_SECTOR_SIZE), .ctx = image};
    return true;
}
static fat32_volume_t *mount_backend(blkdev_t *backend) {
    fat32_volume_t *volume = NULL;
    CHECK(fat32_mount(backend, &volume) == 0 && volume != NULL, "valid FAT32 mounted");
    return volume;
}
static void check_text(fat32_volume_t *volume, const char *path, const char *expected) {
    fat32_file_t file;
    CHECK(fat32_open(volume, path, &file) == 0, "open %s", path);
    char content[64] = {0};
    long count = fat32_read(&file, content, sizeof(content) - 1);
    CHECK(count > 0 && strcmp(content, expected) == 0, "content belongs to correct volume");
    CHECK(fat32_read(&file, content, sizeof(content)) == 0, "file EOF is stable");
    fat32_close(&file);
}
static void volume_interleaving(void) {
    blkdev_t alpha, beta;
    if (!load_backend("alpha.img", &alpha) || !load_backend("beta.img", &beta)) return;
    fat32_volume_t *a = mount_backend(&alpha), *b = mount_backend(&beta);
    if (!a || !b) return;
    CHECK(strcmp(fat32_label(a), "alpha") == 0 && strcmp(fat32_label(b), "beta") == 0,
          "root-directory labels override stale BPB label");
    fat32_file_t files[2];
    CHECK(fat32_open(a, "/large.bin", &files[0]) == 0 &&
          fat32_open(b, "/LARGE.BIN", &files[1]) == 0, "large files opened independently");
    uint8_t content[137];
    for (unsigned pos = 0; pos < 5000;) {
        unsigned count = 5000 - pos;
        if (count > sizeof(content)) count = sizeof(content);
        for (unsigned i = 0; i < 2; i++) {
            CHECK(fat32_read(&files[i], content, count) == (long)count, "interleaved read size");
            for (unsigned byte = 0; byte < count; byte++) {
                if (content[byte] != (uint8_t)((pos + byte) * 37 + 11)) {
                    CHECK(false, "interleaved volume data at %u", pos + byte);
                    break;
                }
            }
        }
        pos += count;
    }
    check_text(a, "/marker.txt", "alpha volume\n");
    check_text(b, "/marker.txt", "beta volume\n");
    check_text(a, "/docs/résumé-notes.txt", "alpha volume\n");
    check_text(a, "/a-long-invalid-name.txt", "alpha volume\n");
    fat32_close(&files[0]);
    fat32_close(&files[1]);
    fat32_unmount(b);
    check_text(a, "/marker.txt", "alpha volume\n");
    fat32_unmount(a);
    free(alpha.ctx);
    free(beta.ctx);
    CHECK(live_allocations == 0, "unmount releases volume allocations");
    blkdev_t fallback;
    if (load_backend("bpb-label.img", &fallback)) {
        fat32_volume_t *volume = mount_backend(&fallback);
        CHECK(strcmp(fat32_label(volume), "fallback") == 0, "BPB label used when root label absent");
        fat32_unmount(volume);
        free(fallback.ctx);
    }
}
static void corrupt_media(void) {
    const char *bad_chains[] = {"bad-chain.img", "loop-chain.img"};
    for (unsigned i = 0; i < 2; i++) {
        blkdev_t backend;
        if (!load_backend(bad_chains[i], &backend)) continue;
        fat32_volume_t *volume = mount_backend(&backend);
        fat32_file_t file;
        CHECK(fat32_open(volume, "/large.bin", &file) == 0, "corrupt file entry resolved");
        uint8_t content[5000];
        CHECK(fat32_read(&file, content, sizeof(content)) == FS_ERR_CORRUPT,
              "bad or looping cluster chain rejected");
        fat32_close(&file);
        fat32_unmount(volume);
        free(backend.ctx);
    }
    const char *bad_names[] = {"bad-lfn-checksum.img", "incomplete-lfn.img", "zero-ordinal-lfn.img"};
    for (unsigned i = 0; i < 3; i++) {
        blkdev_t backend;
        if (!load_backend(bad_names[i], &backend)) continue;
        fat32_volume_t *volume = mount_backend(&backend);
        fat32_file_t file;
        CHECK(fat32_open(volume, "/a-long-invalid-name.txt", &file) == FS_ERR_NOTFOUND,
              "invalid LFN cannot produce original long name");
        CHECK(fat32_opendir(volume, "/", &file) == 0, "invalid-LFN directory remains readable");
        fat32_dirent_t entry;
        unsigned entries = 0;
        int rc;
        while ((rc = fat32_readdir(&file, &entry)) == 1) {
            CHECK(strcmp(entry.name, "a-long-invalid-name.txt") != 0,
                  "invalid LFN falls back to short alias");
            entries++;
        }
        CHECK(rc == 0 && entries == 5, "malformed LFN neither hides nor adds real entries");
        fat32_close(&file);
        fat32_unmount(volume);
        free(backend.ctx);
    }
}
// Fixture root directories run on over a 400-cluster chain whose FAT links miss
// the sector cache. The boot-time label scan must stay within a fixed read
// budget and preserve the BPB fallback beyond it.
static void root_label_scan(void) {
    const char *labels[][2] = {{"deep-label.img", "fallback"}, {"late-label.img", "late"}};
    for (unsigned i = 0; i < 2; i++) {
        blkdev_t backend;
        if (!load_backend(labels[i][0], &backend)) continue;
        fat32_volume_t *volume = NULL;
        backend_reads = 0;
        backend_read_limit = 2048;
        int rc = fat32_mount(&backend, &volume);
        backend_read_limit = 0;
        CHECK(rc == 0 && volume != NULL, "long root directory mounts within read budget");
        CHECK(volume && strcmp(fat32_label(volume), labels[i][1]) == 0,
              "root label scan is bounded: %s", labels[i][0]);
        if (volume) fat32_unmount(volume);
        free(backend.ctx);
    }
    const char *bad_roots[] = {"root-loop.img", "root-bad-chain.img"};
    for (unsigned i = 0; i < 2; i++) {
        blkdev_t backend;
        if (!load_backend(bad_roots[i], &backend)) continue;
        fat32_volume_t *volume = NULL;
        CHECK(fat32_mount(&backend, &volume) == FS_ERR_CORRUPT && volume == NULL,
              "corrupt root chain rejected during label scan: %s", bad_roots[i]);
        free(backend.ctx);
    }
    CHECK(live_allocations == 0, "label scan releases all allocations");
}
static void error_paths(void) {
    blkdev_t backend;
    if (!load_backend("bad-geometry.img", &backend)) return;
    fat32_volume_t *volume = NULL;
    CHECK(fat32_mount(&backend, &volume) < 0 && volume == NULL,
          "overflowing geometry rejected without publishing volume");
    free(backend.ctx);
    if (!load_backend("alpha.img", &backend)) return;
    backend_error = BLK_ERR_IO;
    CHECK(fat32_mount(&backend, &volume) == FS_ERR_IO && volume == NULL,
          "mount propagates backend I/O error");
    backend_error = 0;
    fail_allocations = true;
    CHECK(fat32_mount(&backend, &volume) < 0 && volume == NULL,
          "mount allocation failure releases unpublished state");
    fail_allocations = false;
    volume = mount_backend(&backend);
    fat32_file_t file;
    CHECK(fat32_open(volume, "/large.bin", &file) == 0, "file opened before injected error");
    backend_error = BLK_ERR_IO;
    uint8_t content[512];
    CHECK(fat32_read(&file, content, sizeof(content)) == FS_ERR_IO,
          "data I/O error is not reported as EOF");
    backend_error = 0;
    fat32_close(&file);
    fat32_unmount(volume);
    free(backend.ctx);
    CHECK(live_allocations == 0, "failure paths release all allocations");
}

static void namespace_paths(void) {
    blkdev_t alpha, beta;
    if (!load_backend("alpha.img", &alpha) || !load_backend("beta.img", &beta)) return;
    blkdev_registry_reset();
    CHECK(blkdev_register(&alpha, BLKDEV_RAMDISK) == 0 &&
          blkdev_register(&beta, BLKDEV_VIRTIO) == 0, "register independent fixture backends");
    CHECK(fs_mount_registered(blkdev_partition_io(0)) == 0 && fs_volume_count() == 2,
          "namespace mounts all supported volumes");
    CHECK(fs_volume_get(2) == NULL, "volume enumeration bounded");
    CHECK(strcmp(fs_volume_get(0)->device, "df0") == 0 &&
          strcmp(fs_volume_get(1)->device, "df1") == 0, "slots span backend types");
    fs_cwd_t cwd = {0}, location;
    char path[FS_PATH_MAX];
    CHECK(fs_boot_cwd(&cwd) == 0 && fs_getcwd(&cwd, path, sizeof(path)) == 0 &&
          strcmp(path, "alpha:") == 0, "initial cwd is unique boot label root");
    fat32_file_t existing;
    CHECK(fs_open(&cwd, "/marker.txt", &existing) == 0, "file opened on initial volume");
    CHECK(fs_chdir(&cwd, "BeTa:DoCs/ChIlD") == 0 &&
          fs_getcwd(&cwd, path, sizeof(path)) == 0 && strcmp(path, "beta:docs/child") == 0,
          "case-insensitive label and components retain canonical spelling");
    CHECK(fs_resolve(&cwd, ".././child/nested.txt", &location, NULL) == 0,
          "relative parent/dot path resolved on current volume");
    fat32_file_t file;
    CHECK(fs_open(&cwd, ".././child/nested.txt", &file) == 0, "relative file opened");
    char content[64] = {0};
    CHECK(fat32_read(&file, content, sizeof(content) - 1) > 0 &&
          strcmp(content, "beta volume\n") == 0, "relative read uses selected volume");
    CHECK(fat32_read(&existing, content, sizeof(content) - 1) > 0 &&
          strcmp(content, "alpha volume\n") == 0, "existing fd retains original volume after chdir");
    CHECK(fs_chdir(&cwd, "/docs") == 0 && fs_getcwd(&cwd, path, sizeof(path)) == 0 &&
          strcmp(path, "beta:docs") == 0, "leading slash roots at current volume");
    fs_cwd_t child = {0};
    CHECK(fs_cwd_copy(&child, &cwd) == 0, "inherit an independently owned cwd");
    CHECK(fs_namespace_reset() == FS_ERR_BUSY && fs_volume_count() == 2 &&
          fs_assign_count() == 2, "busy reset preserves mounts and default assigns");
    CHECK(fs_getcwd(&cwd, path, sizeof(path)) == 0 && strcmp(path, "beta:docs") == 0,
          "busy reset retains valid cwd ownership");
    CHECK(fs_chdir(&child, "df0:") == 0 && fs_getcwd(&cwd, path, sizeof(path)) == 0 &&
          strcmp(path, "beta:docs") == 0, "inherited cwd is independently mutable");
    CHECK(fs_chdir(&cwd, "missing/..") == FS_ERR_NOTFOUND,
          "missing component cannot be normalized away");
    CHECK(fs_chdir(&cwd, "../marker.txt/..") == FS_ERR_NOTDIR,
          "non-directory intermediate component cannot be normalized away");
    CHECK(fs_chdir(&cwd, "../marker.txt") == FS_ERR_NOTDIR, "chdir rejects files");
    CHECK(fs_chdir(&cwd, "unknown:") == FS_ERR_NOTFOUND, "missing prefix rejected");
    CHECK(fs_getcwd(&cwd, path, sizeof(path)) == 0 && strcmp(path, "beta:docs") == 0,
          "failed chdir preserves cwd");
    CHECK(fs_getcwd(&cwd, path, 3) == FS_ERR_INVAL, "small getcwd buffer rejected");
    CHECK(fs_resolve(&cwd, "df0:marker.txt", &location, NULL) == 0,
          "device-qualified paths select root");
    CHECK(fs_chdir(&cwd, "../../../../") == 0 &&
          fs_getcwd(&cwd, path, sizeof(path)) == 0 && strcmp(path, "beta:") == 0,
          "parent at root remains within current volume");
    CHECK(fs_resolve(&cwd, "docs/bad:component", &location, NULL) == FS_ERR_INVAL,
          "colon inside components rejected");
    CHECK(fs_resolve(&cwd, "", &location, NULL) == FS_ERR_INVAL, "empty path rejected");
    fail_allocations = true;
    CHECK(fs_chdir(&cwd, "docs") < 0, "resolver allocation failure reported");
    fail_allocations = false;
    fat32_close(&existing);
    fat32_close(&file);
    fs_cwd_release(&child);
    fs_cwd_release(&cwd);
    fs_namespace_reset();
    CHECK(live_allocations == 0, "namespace reset releases mounted volumes and scratch state");
    CHECK(fs_mount_registered(NULL) < 0 && fs_boot_cwd(&cwd) < 0,
          "missing preferred root fails despite valid other volumes");
    fs_namespace_reset();
    blkdev_registry_reset();
    free(alpha.ctx);
    free(beta.ctx);
}
static void namespace_collisions(void) {
    const char *second[] = {"duplicate.img", "collision.img"};
    for (unsigned i = 0; i < 2; i++) {
        blkdev_t a, b;
        if (!load_backend("alpha.img", &a) || !load_backend(second[i], &b)) return;
        blkdev_registry_reset();
        CHECK(blkdev_register(&a, BLKDEV_RAMDISK) == 0 &&
              blkdev_register(&b, BLKDEV_SD) == 0, "register collision fixtures");
        CHECK(fs_mount_registered(blkdev_partition_io(0)) == 0, "mount collision fixtures");
        fs_cwd_t cwd = {0};
        char path[64];
        CHECK(fs_boot_cwd(&cwd) == 0, "boot cwd available for collision fixtures");
        if (i == 0) {
            CHECK(fs_getcwd(&cwd, path, sizeof(path)) == 0 && strcmp(path, "df0:") == 0,
                  "duplicate labels make getcwd use device slot");
            CHECK(fs_chdir(&cwd, "alpha:") == FS_ERR_NOTFOUND, "duplicate label lookup ambiguous");
            CHECK(fs_chdir(&cwd, "DF1:") == 0 && fs_getcwd(&cwd, path, sizeof(path)) == 0 &&
                  strcmp(path, "df1:") == 0, "duplicate medium remains accessible by device");
        } else {
            CHECK(fs_chdir(&cwd, "df1:") == 0 && cwd.volume == fs_volume_get(1)->volume,
                  "device names take precedence over labels");
            CHECK(fs_getcwd(&cwd, path, sizeof(path)) == 0 && strcmp(path, "df1:") == 0,
                  "device-label collision produces usable cwd");
        }
        fs_cwd_release(&cwd);
        fs_namespace_reset();
        blkdev_registry_reset();
        free(a.ctx);
        free(b.ctx);
    }
    CHECK(live_allocations == 0, "collision tests leave no allocations");
}

static void namespace_path_boundaries(void) {
    blkdev_t backend;
    if (!load_backend("boundary.img", &backend)) return;
    blkdev_registry_reset();
    CHECK(blkdev_register(&backend, BLKDEV_RAMDISK) == 0, "register maximal path fixture");
    CHECK(fs_mount_registered(blkdev_partition_io(0)) == 0, "mount maximal path fixture");
    fs_cwd_t cwd = {0};
    CHECK(fs_boot_cwd(&cwd) == 0, "boundary fixture boot cwd");
    static char canonical[FS_PATH_MAX];
    unsigned used = 0;
    for (unsigned component = 0; component < 17; component++) {
        canonical[used++] = '/';
        unsigned length = component < 16 ? 254 : 14;
        for (unsigned byte = 0; byte < length; byte++) {
            canonical[used++] = component < 16 ? 'a' : 'b';
        }
    }
    canonical[used] = 0;
    CHECK(used == FS_PATH_MAX - 1, "valid FAT components reach canonical path boundary");
    CHECK(fs_chdir(&cwd, canonical) == 0, "maximal valid canonical path accepted");
    static char qualified[FS_QUALIFIED_PATH_MAX];
    CHECK(fs_getcwd(&cwd, qualified, sizeof(qualified)) == 0,
          "getcwd provides enough space for volume prefix");
    CHECK(fs_chdir(&cwd, qualified) == 0 && strcmp(cwd.path, canonical) == 0,
          "maximal getcwd result round-trips through chdir");
    static char overlong[FS_QUALIFIED_PATH_MAX + 1];
    for (unsigned i = 0; i < FS_QUALIFIED_PATH_MAX; i++) overlong[i] = 'a';
    overlong[FS_QUALIFIED_PATH_MAX] = 0;
    CHECK(fs_chdir(&cwd, overlong) == FS_ERR_INVAL && strcmp(cwd.path, canonical) == 0,
          "overlong qualified input rejected without cwd mutation");
    fs_cwd_release(&cwd);
    fs_namespace_reset();
    blkdev_registry_reset();
    free(backend.ctx);
    CHECK(live_allocations == 0, "boundary path resolution leaks no scratch state");
}

static void assign_semantics(void) {
    blkdev_t alpha, beta;
    if (!load_backend("alpha.img", &alpha) || !load_backend("beta.img", &beta)) return;
    blkdev_registry_reset();
    CHECK(blkdev_register(&alpha, BLKDEV_RAMDISK) == 0 &&
          blkdev_register(&beta, BLKDEV_VIRTIO) == 0, "register assign fixtures");
    CHECK(fs_mount_registered(blkdev_partition_io(0)) == 0, "mount assign fixtures");
    fs_cwd_t cwd = {0}, resolved;
    CHECK(fs_boot_cwd(&cwd) == 0, "assign boot cwd");
    CHECK(fs_assign_count() == 2 && fs_assign_get(2) == NULL, "default assigns enumerated");
    CHECK(fs_volume_get(0)->boot && !fs_volume_get(1)->boot &&
          fs_volume_get(0)->read_only && fs_volume_get(1)->read_only,
          "volume metadata identifies boot and read-only views");
    CHECK(fs_assign_get(0)->immutable && strcmp(fs_assign_get(0)->name, "sys") == 0,
          "sys boot assign immutable");
    CHECK(fs_chdir(&cwd, "beta:docs") == 0, "set different current volume");
    fat32_file_t command;
    CHECK(fs_program_open(&cwd, "bootcmd", &command) == 0 &&
          command.volume == fs_volume_get(0)->volume, "default c selects boot bin independently of cwd");
    fat32_close(&command);
    CHECK(fs_program_open(&cwd, "extrahello", &command) == FS_ERR_NOTFOUND,
          "bare command does not search current volume bin");
    CHECK(fs_assign_set(&cwd, "C:", "beta:bin") == 0, "replace c using case-insensitive name");
    CHECK(fs_program_open(&cwd, "extrahello", &command) == 0 &&
          command.volume == fs_volume_get(1)->volume, "bare command searches new c target");
    fat32_close(&command);
    CHECK(fs_program_open(&cwd, "c:/extrahello", &command) == 0 &&
          command.volume == fs_volume_get(1)->volume, "leading slash after assign retains bin anchor");
    fat32_close(&command);
    CHECK(fs_program_open(&cwd, "bootcmd", &command) == FS_ERR_NOTFOUND,
          "bare command has no hidden boot-bin fallback");
    CHECK(fs_assign_set(&cwd, "c", NULL) == 0 &&
          fs_program_open(&cwd, "bootcmd", &command) == FS_ERR_NOTFOUND,
          "missing c fails instead of using boot bin");
    CHECK(fs_assign_set(&cwd, "c", "sys:bin") == 0, "c restored through sys target");
    CHECK(fs_assign_set(&cwd, "sys", "beta:") < 0 && fs_assign_set(&cwd, "SYS:", NULL) < 0,
          "sys cannot be replaced or removed");
    CHECK(fs_resolve(&cwd, "sys:", &resolved, NULL) == 0 &&
          resolved.volume == fs_volume_get(0)->volume && strcmp(resolved.path, "/") == 0,
          "sys remains the original boot root");
    CHECK(fs_assign_set(&cwd, "WoRk:", ".") == 0 &&
          fs_resolve(&cwd, "work:", &resolved, NULL) == 0 && strcmp(resolved.path, "/docs") == 0,
          "relative target captured against setter cwd");
    CHECK(fs_assign_set(&cwd, "nested", "WORK:/child") == 0,
          "assign slash retains target anchor");
    CHECK(fs_assign_set(&cwd, "work", "alpha:docs") == 0 &&
          fs_resolve(&cwd, "nested:", &resolved, NULL) == 0 &&
          resolved.volume == fs_volume_get(1)->volume && strcmp(resolved.path, "/docs/child") == 0,
          "resolved nested target remains stable after source replacement");
    CHECK(fs_assign_set(&cwd, "work", NULL) == 0 &&
          fs_resolve(&cwd, "nested:", &resolved, NULL) == 0,
          "resolved nested target remains stable after source removal");
    CHECK(fs_assign_set(&cwd, "alpha", "df0:docs") == 0 && fs_chdir(&cwd, "df0:docs") == 0,
          "assign can shadow same-volume label with a subdirectory");
    char path[FS_QUALIFIED_PATH_MAX];
    CHECK(fs_getcwd(&cwd, path, sizeof(path)) == 0 && strcmp(path, "df0:docs") == 0,
          "shadowed subdirectory label makes getcwd use device");
    CHECK(fs_chdir(&cwd, path) == 0 && strcmp(cwd.path, "/docs") == 0,
          "shadowed-label getcwd round-trips without repeating anchor");
    CHECK(fs_assign_set(&cwd, "alpha", NULL) == 0 &&
          fs_getcwd(&cwd, path, sizeof(path)) == 0 && strcmp(path, "alpha:docs") == 0,
          "removing assign restores label addressing");
    CHECK(fs_assign_set(&cwd, "bad", "alpha:marker.txt") == FS_ERR_NOTDIR,
          "file cannot be assign target");
    CHECK(fs_assign_set(&cwd, "bad", "missing:") == FS_ERR_NOTFOUND,
          "missing prefix cannot be assign target");
    CHECK(fs_assign_set(&cwd, "bad", "missing/..") == FS_ERR_NOTFOUND,
          "invalid target component cannot be normalized away");
    const char *bad_names[] = {"", "bad/name", "a:b", "white space", "df0", "DF999"};
    for (unsigned i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
        CHECK(fs_assign_set(&cwd, bad_names[i], "beta:docs") < 0,
              "invalid/reserved assign name rejected: %s", bad_names[i]);
    }
    char too_long[FS_PREFIX_MAX + 2];
    for (unsigned i = 0; i < FS_PREFIX_MAX + 1; i++) too_long[i] = 'a';
    too_long[FS_PREFIX_MAX + 1] = 0;
    CHECK(fs_assign_set(&cwd, too_long, "beta:docs") < 0, "overlong assign name rejected");
    CHECK(fs_assign_set(&cwd, "retained", "beta:docs") == 0, "assign before allocation failure");
    fail_allocations = true;
    CHECK(fs_assign_set(&cwd, "retained", "alpha:docs") < 0, "failed replacement reported");
    fail_allocations = false;
    CHECK(fs_resolve(&cwd, "retained:", &resolved, NULL) == 0 &&
          resolved.volume == fs_volume_get(1)->volume, "failed replacement preserves previous target");
    for (unsigned i = fs_assign_count(); i < FS_MAX_ASSIGNS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "slot%u", i);
        CHECK(fs_assign_set(&cwd, name, "beta:docs") == 0, "fill bounded assign slot %u", i);
    }
    CHECK(fs_assign_count() == FS_MAX_ASSIGNS &&
          fs_assign_set(&cwd, "overflow", "beta:docs") < 0, "assign capacity enforced");
    CHECK(fs_assign_set(&cwd, "retained", "alpha:docs") == 0,
          "replacement remains possible at assign capacity");
    CHECK(fs_assign_set(&cwd, "retained", NULL) == 0 &&
          fs_assign_set(&cwd, "reused", "beta:docs") == 0, "removed assign slot reusable");
    fs_cwd_release(&cwd);
    fs_namespace_reset();
    blkdev_registry_reset();
    free(alpha.ctx);
    free(beta.ctx);
    CHECK(live_allocations == 0, "assign reset releases all target records");
}

static void directory_pin_lifecycle(void) {
    blkdev_t alpha, beta;
    if (!load_backend("alpha.img", &alpha) || !load_backend("beta.img", &beta)) return;
    blkdev_registry_reset();
    CHECK(blkdev_register(&alpha, BLKDEV_RAMDISK) == 0 &&
          blkdev_register(&beta, BLKDEV_VIRTIO) == 0, "register directory-pin fixtures");
    CHECK(fs_mount_registered(blkdev_partition_io(0)) == 0, "mount directory-pin fixtures");
    fat32_volume_t *volume = fs_volume_get(1)->volume;
    fat32_dirent_t parent, nested;
    CHECK(fat32_lookup(volume, "/docs", &parent) == 0 &&
          fat32_lookup(volume, "/docs/child", &nested) == 0, "resolve pinned directory entries");
    fs_cwd_t cwd = {0}, child = {0}, snapshot;
    CHECK(fs_boot_cwd(&cwd) == 0, "owned boot cwd created");
    CHECK(fs_resolve(&cwd, "beta:docs/child", &snapshot, NULL) == 0 &&
          !fat32_directory_busy(volume, nested.first_cluster), "resolution snapshots do not pin directories");
    CHECK(fs_chdir(&cwd, "beta:docs/child") == 0 &&
          fat32_directory_busy(volume, parent.first_cluster) &&
          fat32_directory_busy(volume, nested.first_cluster), "cwd pins selected directory and ancestor");
    CHECK(fs_cwd_copy(&child, &cwd) == 0, "child receives owned ancestor references");
    fs_cwd_release(&cwd);
    CHECK(fat32_directory_busy(volume, parent.first_cluster), "child retains ancestor after parent release");
    fail_allocations = true;
    CHECK(fs_chdir(&child, "alpha:") < 0, "failed chdir cannot discard old pins");
    fail_allocations = false;
    CHECK(fat32_directory_busy(volume, nested.first_cluster), "failed chdir retains current directory pin");
    fs_cwd_release(&child);
    CHECK(!fat32_directory_busy(volume, parent.first_cluster) &&
          !fat32_directory_busy(volume, nested.first_cluster), "last cwd release removes ancestor pins");
    CHECK(fs_boot_cwd(&cwd) == 0 && fs_assign_set(&cwd, "held", "beta:docs/child") == 0 &&
          fat32_directory_busy(volume, parent.first_cluster) &&
          fat32_directory_busy(volume, nested.first_cluster), "assign target pins its ancestor chain");
    fail_allocations = true;
    CHECK(fs_assign_set(&cwd, "held", "alpha:docs") < 0, "failed assign replacement reported");
    fail_allocations = false;
    CHECK(fat32_directory_busy(volume, nested.first_cluster), "failed assign replacement retains pins");
    CHECK(fs_assign_set(&cwd, "held", NULL) == 0 &&
          !fat32_directory_busy(volume, parent.first_cluster), "assign removal releases ancestors");
    CHECK(fs_assign_set(&cwd, "c", "beta:docs/child") == 0 &&
          fat32_directory_busy(volume, nested.first_cluster), "command assign owns target pins");
    CHECK(fs_assign_set(&cwd, "c", "sys:bin") == 0 &&
          !fat32_directory_busy(volume, nested.first_cluster), "command assign replacement releases old pins");
    fat32_file_t handle;
    CHECK(fat32_opendir(volume, "/docs/child", &handle) == 0 &&
          fat32_directory_busy(volume, nested.first_cluster), "directory handle holds mutation guard");
    fs_cwd_release(&cwd);
    CHECK(fs_namespace_reset() == FS_ERR_BUSY && fs_volume_count() == 2 &&
          fs_assign_count() == 2, "direct FAT handle prevents reset without dropping namespace state");
    fat32_close(&handle);
    CHECK(!fat32_directory_busy(volume, nested.first_cluster), "closing directory releases mutation guard");
    fs_cwd_release(&cwd);
    CHECK(fs_namespace_reset() == 0, "namespace reset succeeds after last external owner release");
    blkdev_registry_reset();
    free(alpha.ctx);
    free(beta.ctx);
    CHECK(live_allocations == 0, "directory pins and assigns release every reference");
}
static void namespace_sync_failure(void) {
    blkdev_t backend;
    if (!load_backend("alpha.img", &backend)) return;
    // A forced-small FAT32 copy is adequate for reset fault logic, without a
    // filesystem-integrity claim. Real write integrity uses the 64 MiB suite.
    backend.read_only = false;
    backend.write = memory_write;
    backend.flush = memory_flush;
    blkdev_registry_reset();
    CHECK(blkdev_register(&backend, BLKDEV_VIRTIO) == 0 &&
          fs_mount_registered(blkdev_partition_io(0)) == 0, "mount writable reset-failure fixture");
    fat32_volume_t *volume = fs_volume_get(0)->volume;
    fat32_file_t file;
    CHECK(fat32_open_flags(volume, "/large.bin", FAT32_O_RDWR, &file) == 0 &&
          fat32_write(&file, "x", 1) == 1, "dirty media before reset flush fault");
    fat32_close(&file);
    backend_flush_error = BLK_ERR_IO;
    CHECK(fs_namespace_reset() == FS_ERR_IO && fs_volume_count() == 1 &&
          fs_assign_count() == 2, "failed reset sync preserves mounts and assign ownership");
    backend_flush_error = 0;
    fs_cwd_t cwd = {0}, snapshot;
    CHECK(fs_boot_cwd(&cwd) == 0 && fs_resolve(&cwd, "sys:", &snapshot, NULL) == 0 &&
          snapshot.volume == volume, "namespace remains usable after failed reset");
    CHECK(fs_program_open(&cwd, "bootcmd", &file) == 0, "command assign retained after failed sync");
    fat32_close(&file);
    fs_cwd_release(&cwd);
    CHECK(fs_namespace_reset() == 0, "retry reset sync succeeds");
    blkdev_registry_reset();
    free(backend.ctx);
    CHECK(live_allocations == 0, "failed reset retry releases all namespace ownership");
}
static void sync_all_volumes(void) {
    blkdev_t alpha, beta, readonly;
    if (!load_backend("alpha.img", &alpha) || !load_backend("beta.img", &beta) ||
        !load_backend("bpb-label.img", &readonly)) return;
    alpha.read_only = beta.read_only = false;
    alpha.write = beta.write = memory_write;
    alpha.flush = beta.flush = selective_flush;
    observed_sync_backend = &beta;
    observed_sync_flushes = 0;
    failing_sync_backend = NULL;
    blkdev_registry_reset();
    CHECK(blkdev_register(&alpha, BLKDEV_VIRTIO) == 0 &&
          blkdev_register(&beta, BLKDEV_VIRTIO) == 0 &&
          blkdev_register(&readonly, BLKDEV_RAMDISK) == 0 &&
          fs_mount_registered(blkdev_partition_io(0)) == 0, "mount mixed sync-all media");
    CHECK(fs_volume_count() == 3 && fs_sync_all() == 0,
          "clean read-only and unsupported volumes allow harmless sync");
    fat32_file_t file;
    for (unsigned i = 0; i < 2; i++) {
        CHECK(fat32_open_flags(fs_volume_get(i)->volume, "/large.bin", FAT32_O_RDWR, &file) == 0 &&
              fat32_write(&file, i ? "B" : "A", 1) == 1, "dirty independent sync-all volumes");
        fat32_close(&file);
    }
    unsigned before = observed_sync_flushes;
    failing_sync_backend = &alpha;
    CHECK(fs_sync_all() == FS_ERR_IO && observed_sync_flushes > before,
          "sync reports first failure and still flushes later volumes");
    fat32_volume_t *cold = mount_backend(&beta);
    char content = 0;
    CHECK(fat32_open(cold, "/large.bin", &file) == 0 && fat32_read(&file, &content, 1) == 1 && content == 'B',
          "later volume bytes are durable after another volume fails");
    fat32_close(&file);
    CHECK(fat32_unmount(cold) == 0, "release independent durable remount");
    before = observed_sync_flushes;
    CHECK(fat32_sync_volume(fs_volume_get(1)->volume) == 0 && observed_sync_flushes == before,
          "successful volume is clean despite earlier sync failure");
    failing_sync_backend = NULL;
    CHECK(fs_sync_all() == 0 && fs_namespace_reset() == 0, "failed volume sync retries without losing mounted state");
    observed_sync_backend = NULL;
    blkdev_registry_reset();
    free(alpha.ctx);
    free(beta.ctx);
    free(readonly.ctx);
    CHECK(live_allocations == 0, "sync-all cleanup releases every volume and assign");
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    fixture_directory = argv[1];
    volume_interleaving();
    corrupt_media();
    root_label_scan();
    error_paths();
    namespace_paths();
    namespace_collisions();
    namespace_path_boundaries();
    assign_semantics();
    directory_pin_lifecycle();
    namespace_sync_failure();
    sync_all_volumes();
    printf("filesystem_test: %d checks, %d failures\n", test_checks, test_failures);
    return test_failures != 0;
}
