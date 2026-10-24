# Writing a Userland Program

This describes how to write, build, and run a userland program for KoraOS **as
it is today**: there is no on-device compiler and no libc. Programs are
cross-compiled on the host into position-independent ELF executables, installed
onto the FAT32 image under `/bin`, and loaded from there at runtime.

## The environment you get

A program runs in EL0 with a deliberately small runtime:

- **No libc.** No `printf`, no `<string.h>`. You have the syscalls (see
  [syscalls.md](syscalls.md)), a handful of inline helpers in
  [`user/libk/koraos.h`](../user/libk/koraos.h) (`kputs`, `kput_int`,
  `kstrlen`, `kstreq`), `memset`/`memcpy`/`memmove`, and a heap. Anything
  else, you write yourself.
- **No floating point in C, for now.** Programs are built with
  `-mgeneral-regs-only` (the libc port will lift this). The kernel does keep
  each task's FP/SIMD registers, so hand-written assembly can use them; see
  [`tests/user/fpregs.S`](../tests/user/fpregs.S).
- **A ~4 KB stack.** A program's main thread gets a single-page user stack
  (other threads get 16 KB), so keep large buffers off the stack (use `malloc`,
  or static arrays).
- **A heap that grows with the system's free memory.** `malloc`, `free`,
  `calloc` and `realloc` (16-byte aligned) come from
  [`user/libk/malloc.c`](../user/libk/malloc.c), a TLSF allocator over 64 KB
  pools of pages; blocks over 256 KB get page runs of their own. For whole
  pages, call `alloc_pages`/`free_pages` directly. Whatever you do not free is
  reclaimed when your program is reaped. `kheap_info()` reports the heap's
  pools and usage. The heap is shared by all your threads, behind one mutex.
- **A flat identity map.** Pointers are physical addresses; e.g. `fb_info()`
  hands you the framebuffer address and you write pixels to it directly.
- **Preemptive processes on every core.** Each program runs on one core for
  life, the least-loaded one unless you choose (`SPAWN_CORE(n)`, or `cmd @n`
  in the shell); programs on different cores run in parallel, and those on one
  core get 10 ms time slices in turn. `ps` lists them. `spawn` runs a child to completion before returning (then `wait` reaps
  it); `spawn_flags(..., SPAWN_NOWAIT)` starts it in the background, and
  `waitpid(-1, &code, WNOHANG)` collects finished children without blocking.
  `msleep(ms)` sleeps; `uptime_us()` reads a microsecond clock without a
  syscall. `forbid()`/`permit()` keep the CPU and the kernel to yourself while
  you poke at kernel structures (see [locking.md](locking.md)). `kill(pid)`
  (the shell's `kill <pid>`) ends another program.
- **Threads and jobs.** `thread_create(fn, arg, core)` runs `long fn(void
  *arg)` in a new thread of your program, on `core` for life (`-1`: the
  least-loaded core); everything but the stack and registers is shared.
  `thread_join(tid, &result)` waits for it and collects `fn`'s return value
  (or `thread_exit(value)`'s). Guard shared data with a `mutex_t`
  (`mutex_lock`/`mutex_unlock`). On top, the TempleOS-style job API queues a
  function to a core and collects the result later: `job_t *job =
  job_queue(fn, arg, core)`, then `long r = job_result(job)`. `exit()` from any
  thread (or returning from `main`) ends the whole program, all threads
  included; if the main thread calls `thread_exit` instead, the program runs
  on until its last thread ends. About 32 tasks exist system-wide, so join
  threads before starting many more. `sysinfo()` tells how many cores are
  online. For lock-free counters and flags, libk has atomics on any `int`,
  `long` or pointer: `atomic_get`/`atomic_set` (acquire/release),
  `atomic_add` (returns the new value), `atomic_inc`/`atomic_dec`,
  `atomic_swap` and `atomic_cas`, plus `cpu_relax()` for spin loops.
  [`user/jobdemo.c`](../user/jobdemo.c) and [`user/pindemo.c`](../user/pindemo.c)
  are worked examples of jobs and of pinned sub-processes.
- **`main` signature.** Either `int main(void)` or `int main(int argc, char
  **argv)`; the entry stub [`user/crt0.S`](../user/crt0.S) calls `main` and
  passes its return value to `exit`.

## The smallest program

```c
/* user/greet.c */
#include "libk/koraos.h"

int main(int argc, char **argv) {
    kputs("hello");
    for (int i = 1; i < argc; i++) {
        kputs(" ");
        kputs(argv[i]);
    }
    kputs("\n");
    return 0;
}
```

`kputs` is just `write(1, s, kstrlen(s))`. Look at the existing programs for
patterns: [`user/hello.c`](../user/hello.c) (minimal),
[`user/echo.c`](../user/echo.c) (argv), [`user/cat.c`](../user/cat.c) and
[`user/ls.c`](../user/ls.c) (the file syscalls),
[`user/init.c`](../user/init.c) (spawning), and
[`user/gfxdemo.c`](../user/gfxdemo.c) (framebuffer).

## Building it

Register the program in [`cmake/userfs.cmake`](../cmake/userfs.cmake), inside the
`koraos_add_userfs` function, next to the existing registrations:

```cmake
koraos_add_user_program(greet "${USER_DIR}/greet.c")
```

That is all. `koraos_add_user_program` compiles the source together with the userland
runtime (`crt0.S` + `syscall.S`) into a standalone **position-independent** ELF
(`-fPIE -Wl,-pie`), using the same cross toolchain as the kernel:

```
--target=aarch64-none-elf -mcpu=cortex-a72 -ffreestanding -nostdlib
-fno-builtin -fno-stack-protector -mgeneral-regs-only -fPIE -O2
```

The linker is told to emit classic `DT_RELA` relocations
(`--pack-dyn-relocs=none`), because the kernel's ELF loader
([`src/user/elf.c`](../src/user/elf.c)) only handles `ET_DYN` images with
`R_AARCH64_RELATIVE` relocations, no dynamic linking, no other reloc types.

Build as usual (`mtools` is required so the FS image can be assembled):

```bash
./build.sh --target qemu_virt
```

CMake installs `build/userfs/aarch64/user/greet.elf` onto the FAT32 image as `/bin/greet` (the
`.elf` suffix is stripped) and rebuilds the embedded image automatically. See
[filesystem.md](filesystem.md) for how the image is put together.

## Running it

Boot and use the shell:

```bash
./run-qemu.sh --target qemu_virt
```

```
$ greet world
hello world
$ ls /bin
```

The shell resolves a bare command name to `/bin/<name>` and `spawn`s it, so
`greet world` runs `/bin/greet` with `argv = {"greet", "world"}`. You can also
spawn by absolute path.

## Shipping non-program files

To put data files on the image (not programs), drop them under
[`fsroot/`](../fsroot); the tree is mirrored into the image root. Read them from
a program with `open` / `read` / `close`.

## Constraints to keep in mind

- Names/paths are **UTF-8**; a `struct dirent` name can be up to 765 bytes.
- `argv` is capped at 16 entries.
- A program the shell runs in the foreground keeps the shell waiting until it
  exits; run long jobs with `&`. Only one program should read the console at
  a time: concurrent readers would take turns getting characters.
- The filesystem is **read-only**, a program cannot create or modify files yet.
