#include "sys/syscall.h"
#include "arch/trapframe.h"
#include "common.h"
#include "fs/fat32.h"
#include "lib/string.h"
#include "proc/task.h"
#include "proc/user_mem.h"
#include "tty.h"
#include "mm.h"
#include "mm/frame_alloc.h"
#include "mm/kmalloc.h"
#include "video/console_fb.h"

// lseek whence values; must match user/libk/koraos.h.
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Kernel-side mirrors of the userland struct dirent / struct stat
// (user/libk/koraos.h). Layouts must match exactly: the syscalls fill these
// through user pointers.
struct kdirent {
    uint64_t size;
    uint32_t is_dir;
    char name[FAT32_NAME_MAX + 1];
};

struct kstat {
    uint64_t size;
    uint32_t is_dir;
};

// Namespace enumeration ABI mirrors user/libk/koraos.h.
struct kvolume_info {
    char device[8];
    char label[12];
    uint32_t flags;
};

struct kassign_info {
    char name[32];
    char target[FS_QUALIFIED_PATH_MAX];
    uint32_t flags;
};

_Static_assert(sizeof(struct kvolume_info) == 24, "volume info ABI");
_Static_assert(sizeof(struct kassign_info) == 4164, "assign info ABI");

// Kernel-side mirror of the userland struct fb_info (user/libk/koraos.h). The
// layout must match exactly: the syscall fills this through a user pointer.
struct fb_info {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};

// Rough sanity check for a user-supplied pointer + length. The flat identity map
// gives no real isolation, so this only rejects null/wild pointers and ranges
// that overflow or fall outside the low-4GB identity map -- enough to turn a bad
// user pointer into an error return instead of a kernel fault.
static bool uptr_ok(uint64_t addr, uint64_t len) {
    if (addr == 0) {
        return false;
    }
    uint64_t end = addr + len;
    if (end < addr) {
        return false;  // overflow
    }
    if (end > 0x100000000ULL) {
        return false;  // beyond the identity map
    }
    return true;
}

// Copy bounded filesystem paths before resolving them. Each byte is checked
// against the current flat-map ABI, including the terminating NUL.
static char *copy_user_path(const char *path) {
    char *copy = kmalloc(FS_QUALIFIED_PATH_MAX);
    if (copy == NULL) {
        return NULL;
    }
    for (unsigned i = 0; i < FS_QUALIFIED_PATH_MAX; i++) {
        if (!uptr_ok((uint64_t)path + i, 1)) {
            break;
        }
        copy[i] = path[i];
        if (copy[i] == 0) {
            return copy;
        }
    }
    kfree(copy);
    return NULL;
}

// Look up an open file/directory handle by fd (>= FD_BASE) in the current task,
// or NULL if the fd is out of range or not open.
static open_file_t *fd_lookup(int fd) {
    task_t *t = task_current();
    if (t == NULL || fd < FD_BASE || fd >= FD_BASE + MAX_OPEN_FILES) {
        return NULL;
    }
    open_file_t *of = &t->files[fd - FD_BASE];
    return of->used ? of : NULL;
}

// Preserve terminal writes on fd 1/2; file descriptors use their own access
// mode and shared inode metadata. Lengths are bounded before the FAT API cast.
static long sys_write(int fd, const char *buf, uint64_t len) {
    if (fd == 1 || fd == 2) {
        if (!uptr_ok((uint64_t)buf, len)) return -1;
        for (uint64_t i = 0; i < len; i++) tty_putc(buf[i]);
        tty_flush();
        return (long)len;
    }
    open_file_t *of = fd_lookup(fd);
    if (of == NULL || of->file.is_dir || !(of->file.flags & 3u) || len > 0xffffffffu ||
        (len && !uptr_ok((uint64_t)buf, len))) return -1;
    long count = fat32_write(&of->file, buf, (uint32_t)len);
    return count < 0 ? -1 : count;
}

// Read a line from the terminal, echoing as it goes and honouring backspace.
// Returns at a newline or when the buffer fills. Other control bytes are not
// echoed, and escape sequences (arrow keys from a serial terminal, or the
// terminal's own status replies) are swallowed whole: there is no line editing
// to give them meaning yet.
static long read_console_line(char *buf, uint64_t len) {
    uint64_t i = 0;
    enum { ESC_NONE, ESC_START, ESC_BODY } esc = ESC_NONE;
    while (i < len) {
        char c = tty_getc();
        if (c == 0x1b) {
            esc = ESC_START;
            continue;
        }
        if (esc == ESC_START) {
            // ESC [ ... and ESC O x run to a final byte; other ESC x are done.
            esc = (c == '[' || c == 'O') ? ESC_BODY : ESC_NONE;
            continue;
        }
        if (esc == ESC_BODY) {
            if (c >= 0x40 && c <= 0x7e) {
                esc = ESC_NONE;
            }
            continue;
        }
        if (c == '\r') {
            c = '\n';
        }
        if (c == 0x7f || c == '\b') {  // delete / backspace
            if (i > 0) {
                i--;
                tty_putc('\b');
                tty_putc(' ');
                tty_putc('\b');
            }
            continue;
        }
        if (c != '\n' && (c < 0x20 || c > 0x7e)) {
            continue;
        }
        tty_putc(c);  // echo
        buf[i++] = c;
        if (c == '\n') {
            break;
        }
    }
    tty_flush();
    return (long)i;
}

// read(fd, buf, len): fd 0 (stdin) reads a console line; fd >= FD_BASE reads
// from an open file. Returns bytes read, or -1.
static long sys_read(int fd, char *buf, uint64_t len) {
    if (fd == 0) {
        if (len == 0) return 0;
        if (!uptr_ok((uint64_t)buf, len)) return -1;
        return read_console_line(buf, len);
    }
    open_file_t *of = fd_lookup(fd);
    if (of == NULL || of->file.is_dir || (of->file.flags & 3u) == FAT32_O_WRONLY ||
        len > 0xffffffffu || (len && !uptr_ok((uint64_t)buf, len))) return -1;
    long count = fat32_read(&of->file, buf, (uint32_t)len);
    return count < 0 ? -1 : count;
}

// open(path, flags): resolve against the caller's cwd and reserve an fd
// before any creation/truncation. Directory handles require pure O_RDONLY.
static long sys_open(const char *path, int flags) {
    uint32_t mode = (uint32_t)flags;
    if ((mode & ~(3u | FAT32_O_CREAT | FAT32_O_TRUNC | FAT32_O_APPEND | FAT32_O_EXCL)) ||
        (mode & 3u) == 3u || (!(mode & 3u) && (mode & (FAT32_O_TRUNC | FAT32_O_APPEND))) ||
        ((mode & FAT32_O_EXCL) && !(mode & FAT32_O_CREAT))) return -1;
    if (!uptr_ok((uint64_t)path, 1)) {
        return -1;
    }
    task_t *t = task_current();
    if (t == NULL) {
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!t->files[i].used) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return -1;  // fd table full
    }
    char *copy = copy_user_path(path);
    if (copy == NULL) {
        return -1;
    }
    fat32_file_t f;
    int rc = fs_open_flags(&t->cwd, copy, mode, &f);
    if (rc == FS_ERR_ISDIR && mode == FAT32_O_RDONLY) {
        rc = fs_opendir(&t->cwd, copy, &f);  // a directory: open it for readdir
    }
    kfree(copy);
    if (rc != 0) {
        return -1;
    }
    t->files[idx].file = f;
    t->files[idx].used = true;
    return FD_BASE + idx;
}

static long sys_close(int fd) {
    open_file_t *of = fd_lookup(fd);
    if (of == NULL) {
        return -1;
    }
    fat32_close(&of->file);
    of->used = false;
    return 0;
}

// lseek(fd, offset, whence): reposition a file's cursor within [0, size].
static long sys_lseek(int fd, long offset, int whence) {
    open_file_t *of = fd_lookup(fd);
    if (of == NULL || of->file.is_dir) {
        return -1;
    }
    long pos = fat32_seek(&of->file, offset, whence);
    return pos < 0 ? -1 : pos;
}

// readdir(fd, out): fetch the next entry from an open directory. Returns 1 for
// an entry, 0 at end of directory, or -1 on error.
static long sys_readdir(int fd, struct kdirent *out) {
    open_file_t *of = fd_lookup(fd);
    if (of == NULL || !of->file.is_dir) {
        return -1;
    }
    if (!uptr_ok((uint64_t)out, sizeof(*out))) {
        return -1;
    }
    fat32_dirent_t de;
    int r = fat32_readdir(&of->file, &de);
    if (r != 1) {
        return r < 0 ? -1 : 0;
    }
    out->size = de.size;
    out->is_dir = de.is_dir;
    memcpy(out->name, de.name, sizeof(out->name));
    return 1;
}

// stat(path, out): report the size and type of an absolute path.
static long sys_stat(const char *path, struct kstat *out) {
    if (!uptr_ok((uint64_t)path, 1) || !uptr_ok((uint64_t)out, sizeof(*out))) {
        return -1;
    }
    task_t *t = task_current();
    char *copy = copy_user_path(path);
    if (t == NULL || copy == NULL) {
        kfree(copy);
        return -1;
    }
    fat32_stat_t st;
    int rc = fs_stat(&t->cwd, copy, &st);
    kfree(copy);
    if (rc != 0) {
        return -1;
    }
    out->size = st.size;
    out->is_dir = st.is_dir;
    return 0;
}

static long sys_chdir(const char *path) {
    task_t *t = task_current();
    char *copy = copy_user_path(path);
    if (t == NULL || copy == NULL) {
        kfree(copy);
        return -1;
    }
    int rc = fs_chdir(&t->cwd, copy);
    kfree(copy);
    return rc == 0 ? 0 : -1;
}

static long sys_getcwd(char *buf, uint64_t size) {
    task_t *t = task_current();
    if (t == NULL || !uptr_ok((uint64_t)buf, size)) {
        return -1;
    }
    return fs_getcwd(&t->cwd, buf, (size_t)size) == 0 ? 0 : -1;
}

static long sys_volume_info(unsigned index, struct kvolume_info *out) {
    if (!uptr_ok((uint64_t)out, sizeof(*out))) {
        return -1;
    }
    const fs_volume_info_t *info = fs_volume_get(index);
    if (info == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->device, info->device, (size_t)strlen(info->device) + 1);
    memcpy(out->label, info->label, (size_t)strlen(info->label) + 1);
    out->flags = (info->boot ? 1u : 0u) | (info->read_only ? 2u : 0u);
    return 1;
}

static long sys_assign_info(unsigned index, struct kassign_info *out) {
    if (!uptr_ok((uint64_t)out, sizeof(*out))) {
        return -1;
    }
    const fs_assign_info_t *info = fs_assign_get(index);
    if (info == NULL) {
        return 0;
    }
    if (fs_getcwd(&info->target, out->target, sizeof(out->target)) != 0) {
        return -1;
    }
    memset(out->name, 0, sizeof(out->name));
    memcpy(out->name, info->name, (size_t)strlen(info->name) + 1);
    out->flags = info->immutable ? 1u : 0u;
    return 1;
}

static long sys_assign(const char *name, const char *target) {
    task_t *t = task_current();
    if (t == NULL) {
        return -1;
    }
    char *name_copy = copy_user_path(name);
    char *target_copy = target != NULL ? copy_user_path(target) : NULL;
    if (name_copy == NULL || (target != NULL && target_copy == NULL)) {
        kfree(name_copy);
        kfree(target_copy);
        return -1;
    }
    int rc = fs_assign_set(&t->cwd, name_copy, target_copy);
    kfree(name_copy);
    kfree(target_copy);
    return rc == 0 ? 0 : -1;
}

static long sys_path_mutation(const char *path, int (*operation)(const fs_cwd_t *, const char *)) {
    task_t *t = task_current();
    if (t == NULL) return -1;
    char *copy = copy_user_path(path);
    if (copy == NULL) return -1;
    int rc = operation(&t->cwd, copy);
    kfree(copy);
    return rc == 0 ? 0 : -1;
}

static long sys_rename(const char *old_path, const char *new_path) {
    task_t *t = task_current();
    if (t == NULL) return -1;
    char *old_copy = copy_user_path(old_path);
    char *new_copy = copy_user_path(new_path);
    if (old_copy == NULL || new_copy == NULL) {
        kfree(old_copy);
        kfree(new_copy);
        return -1;
    }
    int rc = fs_rename(&t->cwd, old_copy, new_copy);
    kfree(old_copy);
    kfree(new_copy);
    return rc == 0 ? 0 : -1;
}

// alloc_pages(count): give the calling task `count` contiguous zeroed pages,
// tracked so they are reclaimed when the task is reaped. Returns the base
// address, or 0 on failure.
static long sys_alloc_pages(uint64_t count) {
    task_t *t = task_current();
    if (t == NULL || count > frame_alloc_total_count()) {
        return 0;
    }
    return (long)(uintptr_t)user_pages_alloc(t, (size_t)count);
}

// free_pages(base): return a run from alloc_pages. Returns 0, or -1 if the
// caller owns no run starting at `base`.
static long sys_free_pages(uint64_t base) {
    task_t *t = task_current();
    if (t == NULL) {
        return -1;
    }
    return user_pages_free(t, (void *)(uintptr_t)base);
}

// spawn(name, argc, argv): load and run an embedded program with arguments.
// spawn(name, argc, argv, flags): load a program and run it as a child; with
// SPAWN_NOWAIT return at once (a background job), otherwise once it exits.
static long sys_spawn(const char *name, int argc, char *const argv[], uint64_t flags) {
    if (!uptr_ok((uint64_t)name, 1) ||
        (flags & ~(uint64_t)(SPAWN_NOWAIT | SPAWN_CORE_MASK)) != 0) {
        return -1;
    }
    if (argc < 0) {
        return -1;
    }
    if (argc > 0 && !uptr_ok((uint64_t)argv, (uint64_t)argc * sizeof(char *))) {
        return -1;
    }
    char *copy = copy_user_path(name);
    if (copy == NULL) {
        return -1;
    }
    int pid = task_spawn(copy, argc, argv, (int)flags);
    kfree(copy);
    return pid;
}

// wait(pid, code, flags): reap an exited child (pid, or any for -1). Returns
// its pid with the exit code in *code (code may be NULL), 0 with WAIT_NOHANG
// when no matching child has exited yet, or -1 if there is no such child.
static long sys_wait(int pid, int *code, uint64_t flags) {
    if ((code != NULL && !uptr_ok((uint64_t)code, sizeof(*code))) ||
        (flags & ~(uint64_t)WAIT_NOHANG) != 0) {
        return -1;
    }
    return task_wait_ex(pid, code, (int)flags);
}

// fb_info(out): report the active screen's framebuffer geometry and address so
// a user program can draw straight into it (flat identity map).
static long sys_fb_info(struct fb_info *out) {
    if (!uptr_ok((uint64_t)out, sizeof(*out))) {
        return -1;
    }
    const framebuffer_info_t *fb = screen_framebuffer();
    if (fb == NULL || fb->buffer == NULL) {
        return -1;
    }
    out->addr = (uint64_t)fb->buffer;
    out->width = fb->width;
    out->height = fb->height;
    out->pitch = fb->pitch;
    out->bpp = fb->depth;
    return 0;
}

void syscall_handle(struct trapframe *tf) {
    uint64_t num = tf->regs[8];
    uint64_t a0 = tf->regs[0];
    uint64_t a1 = tf->regs[1];
    uint64_t a2 = tf->regs[2];
    uint64_t a3 = tf->regs[3];
    long ret = -1;

    switch (num) {
    case SYS_write:
        ret = sys_write((int)a0, (const char *)a1, a2);
        break;
    case SYS_exit:
        task_exit((int)a0);  // does not return
        break;
    case SYS_read:
        ret = sys_read((int)a0, (char *)a1, a2);
        break;
    case SYS_spawn_flags:
        ret = sys_spawn((const char *)a0, (int)a1, (char *const *)a2, a3);
        break;
    case SYS_waitpid:
        ret = sys_wait((int)a0, (int *)a1, a2);
        break;
    case SYS_getpid:
        ret = task_getpid();
        break;
    case SYS_yield:
        task_yield();
        ret = 0;
        break;
    case SYS_fb_info:
        ret = sys_fb_info((struct fb_info *)a0);
        break;
    case SYS_open:
        ret = sys_open((const char *)a0, (int)a1);
        break;
    case SYS_close:
        ret = sys_close((int)a0);
        break;
    case SYS_lseek:
        ret = sys_lseek((int)a0, (long)a1, (int)a2);
        break;
    case SYS_readdir:
        ret = sys_readdir((int)a0, (struct kdirent *)a1);
        break;
    case SYS_stat:
        ret = sys_stat((const char *)a0, (struct kstat *)a1);
        break;
    case SYS_chdir:
        ret = sys_chdir((const char *)a0);
        break;
    case SYS_getcwd:
        ret = sys_getcwd((char *)a0, a1);
        break;
    case SYS_alloc_pages:
        ret = sys_alloc_pages(a0);
        break;
    case SYS_free_pages:
        ret = sys_free_pages(a0);
        break;
    case SYS_msleep:
        task_msleep(a0);
        ret = 0;
        break;
    case SYS_forbid:
        ret = task_forbid();
        break;
    case SYS_permit:
        ret = task_permit();
        break;
    case SYS_task_info:
        ret = uptr_ok(a1, sizeof(struct ktask_info))
                  ? task_info_get((unsigned)a0, (struct ktask_info *)a1)
                  : -1;
        break;
    case SYS_volume_info:
        ret = sys_volume_info((unsigned)a0, (struct kvolume_info *)a1);
        break;
    case SYS_assign:
        ret = sys_assign((const char *)a0, (const char *)a1);
        break;
    case SYS_assign_info:
        ret = sys_assign_info((unsigned)a0, (struct kassign_info *)a1);
        break;
    case SYS_sync:
        ret = fs_sync_all() == 0 ? 0 : -1;
        break;
    case SYS_unlink:
        ret = sys_path_mutation((const char *)a0, fs_unlink);
        break;
    case SYS_mkdir:
        ret = sys_path_mutation((const char *)a0, fs_mkdir);
        break;
    case SYS_rmdir:
        ret = sys_path_mutation((const char *)a0, fs_rmdir);
        break;
    case SYS_rename:
        ret = sys_rename((const char *)a0, (const char *)a1);
        break;
    default:
        ret = -1;
        break;
    }

    tf->regs[0] = (uint64_t)ret;
}
