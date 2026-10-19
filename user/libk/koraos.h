/* KoraOS userland C API. Freestanding: no libc, no kernel headers -- everything
 * a user program needs is declared here. Links against user/libk/syscall.S.
 *
 * The calls are shaped like their POSIX namesakes so a fuller libc shim can wrap
 * them later, but the numbering is KoraOS-private (see abi.h). Not every call is
 * serviced by the kernel yet; unimplemented ones currently return -1.
 */
#pragma once

#include "abi.h"

typedef unsigned long size_t;
typedef long ssize_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Framebuffer geometry, filled by fb_info(). With the flat identity map a user
 * program writes pixels directly to `addr`. */
struct fb_info {
    unsigned long addr;   /* framebuffer base (physical == virtual) */
    unsigned int width;
    unsigned int height;
    unsigned int pitch;   /* bytes per row */
    unsigned int bpp;     /* bits per pixel */
};

/* Filesystem. Paths support cwd, volume/device prefixes and assigns. Names
 * are UTF-8. The kernel mirrors these struct layouts in src/sys/syscall.c;
 * keep them in sync. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_EXCL   0x800

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Max name length in UTF-8 bytes incl. NUL (255 UTF-16 units * 3); must match
 * the kernel's FAT32_NAME_MAX + 1. */
#define DIRENT_NAME_MAX 766

struct dirent {
    unsigned long size;
    int is_dir;
    char name[DIRENT_NAME_MAX];
};

struct stat {
    unsigned long size;
    int is_dir;
};

#define KORA_PATH_MAX 4128 // qualified path capacity including NUL
#define VOLUME_BOOT 1u
#define VOLUME_READ_ONLY 2u
#define ASSIGN_IMMUTABLE 1u

struct volume_info {
    char device[8];
    char label[12];
    unsigned int flags;
};

struct assign_info {
    char name[32];
    char target[KORA_PATH_MAX];
    unsigned int flags;
};

/* Console / I/O */
ssize_t write(int fd, const void *buf, size_t len);
ssize_t read(int fd, void *buf, size_t len);

/* Files: access is O_RDONLY, O_WRONLY or O_RDWR, plus optional create,
 * truncate, append and exclusive-create flags. Only pure O_RDONLY opens
 * directories. File cursors stay within EOF; readdir serves directory fds. */
int open(const char *path, int flags);
int close(int fd);
long lseek(int fd, long offset, int whence);
int readdir(int fd, struct dirent *out);   /* 1 = entry, 0 = end, <0 = error */
int stat(const char *path, struct stat *out);
/* Mutations and durable sync return 0 or -1. Rename never overwrites another
 * entry and rejects cross-volume moves. */
int sync(void);
int unlink(const char *path);
int mkdir(const char *path);
int rmdir(const char *path);
int rename(const char *old_path, const char *new_path);

// Change cwd, or copy its volume-qualified canonical spelling (including NUL).
// Both return 0 on success and -1 on failure; failed chdir preserves cwd.
int chdir(const char *path);
int getcwd(char *buf, size_t size);
// Enumerators return 1 for an item, 0 at end, -1 on failure.
int volume_info(unsigned int index, struct volume_info *out);
int assign_info(unsigned int index, struct assign_info *out);
// Assign a directory target; NULL target removes an assign. sys is fixed.
int assign(const char *name, const char *target);

/* Memory. alloc_pages() returns `count` contiguous zeroed pages (NULL on
 * failure); free_pages() takes the base address back (0, or -1 if the run is
 * not yours). Pages a program does not free are reclaimed when it is reaped.
 * malloc() and friends (user/libk/malloc.c) build a heap on top: 16-byte
 * aligned blocks from pools of pages, so the heap grows as long as the system
 * has free pages. */
#define KORAOS_PAGE_SIZE 4096

#ifdef LIBK_HOST_TEST
/* Host unit tests link libk's malloc next to the host libc: rename it so it
 * does not replace the host's own allocator. */
#define malloc libk_malloc
#define free libk_free
#define calloc libk_calloc
#define realloc libk_realloc
#endif

void *alloc_pages(size_t count);
int free_pages(void *base);

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);

/* Heap diagnostics, walked from the pools (for tests and curious programs). */
struct kheap_info {
    size_t pools;          /* page-run pools backing malloc */
    size_t pool_bytes;     /* their total size */
    size_t used_blocks;    /* live malloc blocks in the pools */
    size_t used_bytes;     /* their size, headers included */
    size_t free_bytes;     /* free space in the pools */
    size_t direct_allocs;  /* large blocks with page runs of their own */
    size_t direct_bytes;
    size_t bad_frees;      /* misaligned or double frees ignored */
    size_t corrupt;        /* inconsistencies found in the pools (should be 0) */
};
void kheap_info(struct kheap_info *out);

/* Memory primitives (user/libk/mem.c); the compiler may also emit calls. */
void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);

/* Process control. Scheduling is preemptive: every runnable program gets
 * time slices of one 10 ms tick in turn.
 *
 * spawn_flags() starts a child; without SPAWN_NOWAIT it returns only once the
 * child has exited, with SPAWN_NOWAIT at once (a background job). Either way
 * the child must then be reaped: waitpid() collects child `pid` (or any child
 * for -1), storing its exit code in *code (may be NULL) and returning its pid;
 * with WNOHANG it returns 0 instead of blocking if none has exited yet. Both
 * return -1 on failure (no such program, or no such child). spawn() and wait()
 * are the simple forms: run to completion, then collect the exit code. A child
 * whose parent exits first is reaped by the kernel. */
#define SPAWN_NOWAIT 1
/* Run the child on core n (0..3); without it the kernel picks the online core
 * with the fewest tasks. A task stays on its core for life. */
#define SPAWN_CORE(n) ((((n) + 1) & 0xf) << 8)
#define WNOHANG 1

void exit(int status) __attribute__((noreturn));
int spawn_flags(const char *name, int argc, char *const argv[], int flags);
int waitpid(int pid, int *code, int flags);
int getpid(void);
void yield(void);
void msleep(unsigned long ms);  /* at least ms milliseconds, in 10 ms ticks */

/* forbid()/permit(), after AmigaOS's Forbid()/Permit(): forbid() takes the big
 * kernel lock and keeps it until the matching permit(), so no other core can
 * be inside the kernel meanwhile, and this task is not preempted (interrupts
 * still run). Use it to look at or poke kernel structures safely. Calls nest
 * and return the new depth (permit: -1 if not forbidden). Blocking (read,
 * msleep, wait) breaks the forbid until the task runs again, as Wait() does on
 * the Amiga; exiting ends it. */
int forbid(void);
int permit(void);

/* One task, as task_info() reports it (the `ps` program). */
#define TASK_NAME_MAX 16
enum { TASK_RUNNING = 0, TASK_READY, TASK_SLEEPING, TASK_ZOMBIE };
struct task_info {
    int pid;
    int ppid;                  /* 0: started by the kernel, or orphaned */
    unsigned core;             /* the core it runs on, for life */
    unsigned state;            /* TASK_RUNNING .. TASK_ZOMBIE */
    char name[TASK_NAME_MAX];
};

/* Fill *out with the index-th live task (0, 1, ...): 1 if there is one, 0
 * past the last, -1 on a bad pointer. Not a snapshot across calls: tasks may
 * come and go in between. */
int task_info(unsigned index, struct task_info *out);

static inline int spawn(const char *name, int argc, char *const argv[]) {
    return spawn_flags(name, argc, argv, 0);
}

/* Exit code of child `pid` once it has exited, or -1 if there is no such child. */
static inline int wait(int pid) {
    int code;
    return waitpid(pid, &code, 0) > 0 ? code : -1;
}

/* Microseconds since boot, from the generic timer's virtual counter (readable
 * at EL0, no syscall). */
static inline unsigned long uptime_us(void) {
    unsigned long count, freq;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(count));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return count / freq * 1000000u + count % freq * 1000000u / freq;
}

/* Graphics */
int fb_info(struct fb_info *out);

/* Small freestanding conveniences shared by the demo programs. */
static inline size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static inline int kstreq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static inline void kputs(const char *s) {
    write(1, s, kstrlen(s));
}

static inline void kput_int(long v) {
    if (v < 0) {
        write(1, "-", 1);
        v = -v;
    }
    unsigned long u = (unsigned long)v;
    if (u == 0) {
        write(1, "0", 1);
        return;
    }
    char buf[24];
    int i = 0;
    while (u > 0) {
        buf[i++] = (char)('0' + (u % 10));
        u /= 10;
    }
    char out[24];
    int j = 0;
    while (i > 0) {
        out[j++] = buf[--i];
    }
    write(1, out, (size_t)j);
}
