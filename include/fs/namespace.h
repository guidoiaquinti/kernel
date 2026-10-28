#pragma once

#include "fs/fat32.h"

#define FS_PATH_MAX 4096 // canonical volume-local path capacity, including NUL
#define FS_PREFIX_MAX 31 // capacity budget for future assign names, excluding colon
#define FS_QUALIFIED_PATH_MAX (FS_PATH_MAX + FS_PREFIX_MAX + 1)
#define FS_DEVICE_NAME_MAX 8
#define FS_MAX_ASSIGNS 16

typedef struct fs_directory_pins fs_directory_pins_t;

typedef struct fs_cwd {
    fat32_volume_t *volume;
    fs_directory_pins_t *pins; // owned cwd/assign ancestor refs; resolver snapshots use NULL
    char path[FS_PATH_MAX]; // canonical absolute path within this volume
} fs_cwd_t;

typedef struct fs_volume_info {
    fat32_volume_t *volume;
    char device[FS_DEVICE_NAME_MAX]; // df0, df1, ...; no colon
    const char *label;
    bool boot;
    bool read_only;
} fs_volume_info_t;

typedef struct fs_assign_info {
    char name[FS_PREFIX_MAX + 1];
    fs_cwd_t target; // resolved snapshot, independent of other assigns
    bool immutable;
} fs_assign_info_t;

unsigned fs_assign_count(void);
const fs_assign_info_t *fs_assign_get(unsigned index);
// NULL target removes an assign. sys is immutable; replacing/removing c is
// allowed. Target paths must resolve to existing directories. Names may carry
// one trailing colon. Device-slot names df<digits> remain reserved.
int fs_assign_set(const fs_cwd_t *cwd, const char *name, const char *target);

// Boot/test initialization only. Mounts every valid registered view. Boot must
// identify the exact preferred view; NULL or an invalid boot fails closed.
int fs_mount_registered(blkdev_t *boot);
// As above, with the first mounted volume (in registration order) that has a
// file at `probe` (e.g. "/bin/init") as the boot volume; FS_ERR_NOTFOUND if
// none has.
int fs_mount_registered_search(const char *probe);
// Reject live tasks/handles and retain registry/assign ownership on sync error.
int fs_namespace_reset(void);
unsigned fs_volume_count(void);
const fs_volume_info_t *fs_volume_get(unsigned index);
int fs_boot_cwd(fs_cwd_t *out);
// Boot cwd, chdir and assign targets own directory pins. Clone a cwd into an
// unowned output and release it before task teardown or namespace reset.
// Resolver results are unowned snapshots and need no release.
int fs_cwd_copy(fs_cwd_t *out, const fs_cwd_t *source);
void fs_cwd_release(fs_cwd_t *cwd);

// Every operation takes its caller's cwd explicitly. Resolution checks each
// component before processing a following '..'. Failure leaves cwd unchanged.
// fs_resolve requires an unowned output distinct from cwd.
int fs_resolve(const fs_cwd_t *cwd, const char *path, fs_cwd_t *out,
               fat32_dirent_t *entry);
int fs_chdir(fs_cwd_t *cwd, const char *path);
int fs_getcwd(const fs_cwd_t *cwd, char *buf, size_t size); // 0 or FS_ERR_*
int fs_open(const fs_cwd_t *cwd, const char *path, fat32_file_t *out);
int fs_opendir(const fs_cwd_t *cwd, const char *path, fat32_file_t *out);
int fs_stat(const fs_cwd_t *cwd, const char *path, fat32_stat_t *out);
// Bare commands use c: exclusively; explicit paths use the caller's cwd.
int fs_program_open(const fs_cwd_t *cwd, const char *name, fat32_file_t *out);

// Mutations resolve the caller's cwd explicitly. New leaves are resolved via
// their existing parent; rename rejects cross-volume destinations.
int fs_open_flags(const fs_cwd_t *cwd, const char *path, uint32_t flags, fat32_file_t *out);
int fs_unlink(const fs_cwd_t *cwd, const char *path);
int fs_mkdir(const fs_cwd_t *cwd, const char *path);
int fs_rmdir(const fs_cwd_t *cwd, const char *path);
int fs_rename(const fs_cwd_t *cwd, const char *source, const char *destination);

// Sync every mounted volume, continuing after errors. Clean read-only or
// unsupported media need no flush; pending recovery errors are returned.
int fs_sync_all(void);
