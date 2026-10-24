# System Calls

KoraOS has a small, KoraOS-private syscall ABI. The calls are *shaped* like
their POSIX namesakes to ease a future libc shim, but the numbering is our own.

## Calling convention

`x8` holds the syscall number; arguments go in `x0` to `x3` (AAPCS order);
`svc #0` traps into the kernel; the return value comes back in `x0`.

The numbers are defined in two places that **must stay in sync**:
[`include/sys/syscall.h`](../include/sys/syscall.h) (kernel) and
[`user/libk/abi.h`](../user/libk/abi.h) (userland). Userland C prototypes and
the `struct` layouts are in [`user/libk/koraos.h`](../user/libk/koraos.h); the
assembly stubs are in [`user/libk/syscall.S`](../user/libk/syscall.S). The
kernel dispatches them in [`src/sys/syscall.c`](../src/sys/syscall.c).

## Summary

| # | Name | Prototype | Returns |
|---|------|-----------|---------|
| 0 | `write` | `ssize_t write(int fd, const void *buf, size_t len)` | bytes written, or `-1` |
| 1 | `exit` | `void exit(int status)` | does not return (ends every thread) |
| 2 | `read` | `ssize_t read(int fd, void *buf, size_t len)` | bytes read, `0` at EOF, or `-1` |
| 3 | (retired) | was `sbrk` | `-1` |
| 4 | `spawn` | `int spawn_flags(const char *name, int argc, char *const argv[], int flags)` | child pid, or `-1` |
| 5 | `wait` | `int waitpid(int pid, int *code, int flags)` | child pid, `0` (`WNOHANG`, none yet), or `-1` |
| 6 | `getpid` | `int getpid(void)` | the program's pid (the same in every thread) |
| 7 | `yield` | `void yield(void)` | `0`, after letting other runnable tasks run |
| 8 | `fb_info` | `int fb_info(struct fb_info *out)` | `0`, or `-1` |
| 9 | `open` | `int open(const char *path, int flags)` | fd (≥ 3), or `-1` |
| 10 | `close` | `int close(int fd)` | `0`, or `-1` |
| 11 | `lseek` | `long lseek(int fd, long offset, int whence)` | new offset, or `-1` |
| 12 | `readdir` | `int readdir(int fd, struct dirent *out)` | `1` entry, `0` end, `-1` error |
| 13 | `stat` | `int stat(const char *path, struct stat *out)` | `0`, or `-1` |
| 14 | `alloc_pages` | `void *alloc_pages(size_t count)` | base of `count` zeroed pages, or `NULL` |
| 15 | `free_pages` | `int free_pages(void *base)` | `0`, or `-1` |
| 16 | `msleep` | `void msleep(unsigned long ms)` | `0`, after at least `ms` milliseconds |
| 17 | `forbid` | `int forbid(void)` | new forbid depth |
| 18 | `permit` | `int permit(void)` | new forbid depth, or `-1` if not forbidden |
| 19 | `task_info` | `int task_info(unsigned index, struct task_info *out)` | `1` entry, `0` past the last, `-1` bad pointer |
| 20 | `thread_new` | `int thread_new(void (*start)(void), thread_fn fn, void *arg, int core)` | thread id, or `-1` (use libk's `thread_create`) |
| 21 | `thread_exit` | `void thread_exit(long value)` | does not return |
| 22 | `thread_join` | `int thread_join(int tid, long *value)` | `0`, or `-1` |
| 23 | `kill` | `int kill(int pid)` | `0`, or `-1` if there is no such running program |
| 24 | `sysinfo` | `int sysinfo(struct sysinfo *out)` | `0`, or `-1` bad pointer |
| 32 | `chdir` | `int chdir(const char *path)` | `0`, or `-1` |
| 33 | `getcwd` | `int getcwd(char *buf, size_t size)` | `0`, or `-1` |
| 34 | `volume_info` | `int volume_info(unsigned int index, struct volume_info *out)` | `1` item, `0` end, `-1` error |
| 35 | `assign` | `int assign(const char *name, const char *target)` | `0`, or `-1` |
| 36 | `assign_info` | `int assign_info(unsigned int index, struct assign_info *out)` | `1` item, `0` end, `-1` error |
| 37 | `sync` | `int sync(void)` | `0`, or `-1` |
| 38 | `unlink` | `int unlink(const char *path)` | `0`, or `-1` |
| 39 | `mkdir` | `int mkdir(const char *path)` | `0`, or `-1` |
| 40 | `rmdir` | `int rmdir(const char *path)` | `0`, or `-1` |
| 41 | `rename` | `int rename(const char *old_path, const char *new_path)` | `0`, or `-1` |

## Notes per call

- **`write`**: fd 1 (stdout) and 2 (stderr) go to the console. A writable file
  descriptor at fd 3 or above writes at its cursor; append selects current EOF
  for every call. Returns accepted bytes, possibly a short write on exhaustion,
  or `-1`. `sync` reports durable completion.
- **`read`**: `fd` 0 reads a line from the console (echoed, backspace honoured,
  returns at newline or when the buffer fills). `fd ≥ 3` reads from an open file.
- **`alloc_pages`** / **`free_pages`**: Amiga-style memory: a run of `count`
  contiguous, zeroed 4 KB pages (`KORAOS_PAGE_SIZE`) anywhere in RAM, returned
  by its base address. The kernel records each run against the calling
  program (any of its threads may free it), so `free_pages` needs no size,
  rejects addresses the program does not own, and everything still allocated
  is reclaimed when the program is reaped. libk's
  `malloc` builds on these (see
  [writing-userland-programs.md](writing-userland-programs.md)). There is no
  `sbrk`: with one flat address space, memory after a heap is usually someone
  else's, so a contiguous break cannot grow reliably.
- **`exit`**: ends the whole program, whichever thread calls it (returning
  from `main` calls it). Its other threads are marked killed and die on their
  own cores (see `kill`); the program is finished, and can be reaped, once the
  last of them is gone.
- **`spawn`**: creates a process for the program, with one (main) thread. Without `SPAWN_NOWAIT` (flags
  0) it blocks the caller until the child has exited, then returns the child's
  pid; with `SPAWN_NOWAIT` it returns the pid at once and the child runs
  alongside (a background job). Either way, reap the child with `waitpid`.
  `SPAWN_CORE(n)` runs the child on core `n` for life (it fails if that core
  does not exist or is offline); without it the kernel puts the child on the
  online core with the fewest tasks. libk's `spawn(name, argc, argv)` is the
  flags-0 form. `name` is resolved to a filesystem path: a bare
  name is looked up through `c:` (initially `sys:bin`), with no fallback to
  `/bin` if the assign or command is missing; other names use the caller's
  volume-aware path resolver (see
  [filesystem.md](filesystem.md)). `argv` entries are copied onto the child's
  stack and delivered as `main(argc, argv)`.
- **`wait`**: reaps an exited child, `pid` or any child for `-1`, storing its
  exit code in `*code` (which may be `NULL`) and returning its pid. It blocks
  until one exits, unless `WNOHANG` is set, in which case it returns `0` if
  none has. `-1` means the caller has no such child. Children belong to the
  program, so any of its threads may reap them. libk's `wait(pid)`
  returns the exit code directly. A child whose parent exits first becomes an
  orphan and is reaped by the kernel.
- **`task_info`**: the `index`-th live task, one per thread (0, 1, ...): the
  program's pid, its parent pid (0 for the kernel or an orphan), core, state
  (`TASK_RUNNING`, `TASK_READY`, `TASK_SLEEPING`, `TASK_ZOMBIE` for an exited
  thread), name, and the thread id (`tid`, equal to `pid` for the main
  thread). Each call is consistent; tasks may come and go between calls.
  `/bin/ps` prints the list.
- **`thread_new`**: starts a thread of the calling program at `start` (in
  EL0) with `x0 = fn` and `x1 = arg`, on a fresh 16 KB stack, on `core` for life
  (`-1`: the online core with the fewest tasks). It shares everything else with
  the program: memory, heap, open files, cwd. libk's `thread_create(fn, arg,
  core)` passes its own `start`, which calls `fn(arg)` and hands the result to
  `thread_exit`. Fails for a missing or offline core, when the task table
  (32 tasks system-wide) or memory runs out, or while the program is exiting.
- **`thread_exit`**: ends the calling thread with `value` for `thread_join`.
  The last thread to end finishes the program, with exit code 0 unless `exit`
  or `kill` set one.
- **`thread_join`**: waits until thread `tid` of the calling program has
  ended, stores its value in `*value` (which may be `NULL`) and frees it. Each
  thread can be joined once; `-1` for an unknown thread, one of another
  program, the caller itself, or one already joined. Unjoined threads are
  freed with their program.
- **`kill`**: ends program `pid` as if it had called `exit(EXIT_KILLED)`
  (`-9`). Nothing interrupts another core: each thread notices on its own
  core, on its next syscall or on its way back to EL0 after its tick, and a
  thread blocked in `read`, `msleep`, `wait` or `thread_join` is woken to give
  up its call. A forbidden thread keeps the BKL, so `kill` waits until it
  permits. There are no signals: the program cannot catch it. Its parent
  still reaps it. The shell's `kill <pid>` uses it.
- **`sysinfo`**: the machine at a glance: cores online (use cores `0` to
  `cpus - 1` with `SPAWN_CORE` and `thread_create`) and the most the kernel
  manages, the tick rate and ticks since boot, the page size and the page
  pool's total and free pages, and live tasks and programs (zombies included)
  with their limits.
- **`msleep`**: blocks for at least `ms` milliseconds, in whole 10 ms ticks
  (plus up to one tick); other tasks run meanwhile. `msleep(0)` yields.
- **`forbid`** / **`permit`**: after AmigaOS's `Forbid()`/`Permit()`. `forbid`
  keeps the big kernel lock across returns to EL0 until the matching `permit`,
  so no other core can be in the kernel meanwhile, and stops this task being
  preempted (interrupts still run). They nest; blocking breaks the forbid only
  while the task sleeps; exiting ends it. See [locking.md](locking.md).
- **`open`**: accepts one access mode (`O_RDONLY`, `O_WRONLY`, `O_RDWR`) and
  create/exclusive/truncate/append flags below. Exclusive requires create;
  truncate and append require writable access. Create alone preserves an
  existing file and may create a read-only handle on writable media. Directories
  accept only plain `O_RDONLY`. The kernel reserves a descriptor before
  creating or truncating, so a full table cannot mutate the file. fds 0/1/2
  are the console; real files start at 3.
- **`lseek`**: `whence` is `SEEK_SET` / `SEEK_CUR` / `SEEK_END`; the new offset
  must land within `[0, size]`. Not valid on directory fds.
- **`readdir`**: returns one entry per call from a directory fd. Skips deleted
  entries, the volume label, and `.` / `..`. `name` is UTF-8.
- **`stat`**: reports size and whether the path is a directory.
- **`chdir`**: changes only the calling program's current directory. Supports
  device/volume prefixes, current-volume absolute paths, relative paths, `.` and
  `..`. The target must be a directory; failure preserves cwd. Children inherit
  cwd at spawn and can change it independently.
- **`getcwd`**: writes a NUL-terminated canonical path such as `boot:docs`, using
  a unique volume label or the device slot when that label is ambiguous or
  collides with another namespace name. The supplied byte capacity must include
  the terminator; invalid or undersized buffers return `-1`. Returns `0` on
  success, rather than a pointer.

- **`volume_info`**: enumerate mounted FAT32 volumes by zero-based index. Reports
  the device slot, volume label, boot-volume flag and read-only flag. Returns
  `1` for an item and `0` at the end; invalid output pointers return `-1`.
- **`assign`**: create or replace a global, single-target assign to an existing
  directory. Relative targets use the caller's cwd. A NULL target removes an
  assign; removing an unknown assign fails. `sys:` is immutable; `c:` can be
  replaced or removed. Targets are resolved snapshots, and failed replacements
  preserve the previous target. Device-slot names remain reserved.
- **`assign_info`**: enumerate assigns by zero-based index. Reports the name,
  canonical qualified target and immutable flag. Returns `1` for an item,
  `0` at the end or `-1` for invalid output pointers.

- **`sync`**: flushes every writable mounted volume, including cached data,
  allocation tables, directory metadata and FSInfo. Read-only volumes are
  skipped. It attempts later volumes even if an earlier one failed and returns
  `-1` if any flush failed. Closing a descriptor does not imply sync.
- **`unlink`**: removes a file and its clusters. Directories, read-only files
  and live handles are rejected.
- **`mkdir`**: creates one directory with `.` and `..` entries. Its parent must
  exist; it does not create a chain of missing parents.
- **`rmdir`**: removes an empty directory. Root, cwd/assign pins, live directory
  handles and nonempty directories are rejected.
- **`rename`**: renames within one volume. Existing destination entries are
  preserved, apart from a case-only update of the same source. Open handles,
  cwd/assign pins, cross-volume moves and moving a directory under itself fail.

File I/O validates descriptor permissions even for zero bytes. Valid file
zero-byte calls may use a NULL buffer; console output retains its pointer
checks. File lengths above `UINT32_MAX`, invalid buffers and invalid flags
return `-1`. The kernel API retains detailed errors; user syscalls map them to
`-1`.

File paths for `open`, `stat`, `chdir` and explicit `spawn` paths use the same
namespace rules; `/` selects the current volume's root. Syscall numbers 14 to
31 belong to the scheduler/memory track; filesystem additions use 32 to 47.

## Structs and constants

From [`user/libk/koraos.h`](../user/libk/koraos.h) (mirrored kernel-side in
`src/sys/syscall.c`, keep the layouts identical):

```c
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x100
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_EXCL 0x800
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define DIRENT_NAME_MAX 766   /* UTF-8 bytes incl. NUL (255 UTF-16 units * 3) */

struct dirent {
    unsigned long size;
    int           is_dir;
    char          name[DIRENT_NAME_MAX];
};

struct stat {
    unsigned long size;
    int           is_dir;
};

#define KORA_PATH_MAX 4128  /* qualified path bytes including NUL */
#define VOLUME_BOOT 1u
#define VOLUME_READ_ONLY 2u
#define ASSIGN_IMMUTABLE 1u

struct volume_info {
    char device[8];         /* slot name without colon */
    char label[12];         /* FAT label, or empty */
    unsigned int flags;
};

struct assign_info {
    char name[32];          /* assign name without colon */
    char target[KORA_PATH_MAX];
    unsigned int flags;
};

struct fb_info {           /* filled by fb_info() */
    unsigned long addr;    /* framebuffer base (physical == virtual) */
    unsigned int  width;
    unsigned int  height;
    unsigned int  pitch;   /* bytes per row */
    unsigned int  bpp;     /* bits per pixel */
};
```

## Adding a syscall

1. Add the number to **both** `include/sys/syscall.h` and `user/libk/abi.h`.
2. Add an assembly stub in `user/libk/syscall.S` (`SYSCALL name, SYS_NAME`).
3. Add the C prototype (and any structs) to `user/libk/koraos.h`.
4. Implement `sys_<name>` and add a `case` in `syscall_handle` in
   `src/sys/syscall.c`. Validate user pointers with `uptr_ok`.
