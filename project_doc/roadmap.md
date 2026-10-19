# KoraOS Roadmap

High-level milestones for the kernel and userland. Each milestone is delivered as
a sequence of PRs (one PR per step); detailed per-milestone ledgers can live next
to this file, as `virt-plan.md` does. Update this roadmap as decisions change.
Per-feature status (wish, planned, in progress, completed) lives in
`features.yaml`, which the project website reads.

KoraOS is an open machine: a single flat identity map with no memory protection
between programs or between programs and the kernel. That is the design, not a
gap, and nothing below tries to change it (see `docs/how-userland-works.md`).

## Completed foundations

- **Boot and early bring-up**: boot chain on Pi 3/Pi 4 and QEMU, EL2/EL3 to EL1
  drop, system register helpers, boot diagnostics.
- **Exceptions and interrupts**: vector table, BCM (Pi 3) and GIC-400 / GICv2
  (Pi 4, virt) controllers, generic timer tick (`systick`).
- **Console**: unified UART console, `printf`, framebuffer terminal with
  scrolling, xterm-style escapes and scrollback; UART debug console alongside it.
- **Userland**: EL0 PIE programs loaded by the ELF loader, cooperative nested
  spawn/wait, argv, interactive shell.
- **Filesystem**: read-only FAT32 with long filenames over the `blkdev_t` seam,
  file syscalls, userland loaded from `/bin`.
- **Input and video**: Circle USB stack with HID keyboards on Pi 3 and Pi 4,
  HDMI framebuffer on both.
- **QEMU virt target**: DTB discovery, PL011, GICv2, ramfb, VirtIO block and
  keyboard; see `virt-plan.md`.
- **CI**: host unit tests, QEMU smoke tests (raspi3b and virt profiles), GitHub
  Actions with required checks.

## Milestone order

| # | Milestone | Depends on |
|---|-----------|------------|
| 6 | Kernel heap and user memory | |
| 7 | Scheduler, SMP and threads | 6 |
| 8 | Volumes and namespace | 6.1; 8.2 follows 8.1 |
| 9 | FAT32 write | 8.2 |
| 10 | Storage drivers (SD, USB mass storage) | SD: none; USB: 6.1, 7.5 |
| 11 | Hotplug (USB, HDMI) | 7, 10 |
| 12 | libc port | 6, 7, 8, 9 |

The libc port only depends on milestones 6 to 9, so it can move ahead of 10 and
11 if a real libc becomes the priority. Filesystem milestones 8 and 9 do not
depend on the scheduler: filesystem calls remain serial under the big kernel
lock. SD backends can register through 8.1 when available; SD write enablement
remains separate. USB mass storage needs the core-0 I/O worker after 7.5.
Track B delivery and verification are recorded in `filesystem-plan.md`.

## Milestone 6 – Kernel heap and user memory

Before this milestone the kernel `operator new` (and Circle's `malloc`, which
wraps it) handed out whole pages, so a 16-byte allocation cost 4 KB, and each
task gets a fixed 64 KB `sbrk` heap. Every later milestone needs small,
freeable kernel objects.

1. **Kernel heap** (done): `kmalloc`/`kmalloc_aligned`/`kfree` in
   `src/mm/kmalloc.c`. One-page slabs in nine size classes (64 to 1344 bytes,
   all multiples of 64, so blocks never share a cache line and DMA cache
   maintenance stays safe for Circle); larger or over-aligned requests take
   page runs. Each page starts with a 64-byte header found from the freed
   pointer, so no size is needed on free. Blocks come back zeroed; invalid and
   double frees are caught and counted. IRQ-safe by masking (Circle allocates
   in USB completion handlers), as is the frame allocator now. `operator new`
   (including `std::align_val_t`) and Circle's `malloc` use it. Host unit tests,
   a boot self-test, and the debug console's `heap` and `heaptest` commands,
   run by every QEMU smoke profile.
2. **User memory** (done): Amiga-style `alloc_pages(count)` /
   `free_pages(base)` syscalls (14 and 15) hand out zeroed page runs, recorded
   per task (`src/proc/user_mem.c`) so unfreed runs are reclaimed when the task
   is reaped. With VA == PA a contiguous break cannot grow reliably once
   neighbouring memory is taken, so `sbrk` was removed (number 3 is retired).
   `libk` provides `malloc`/`free`/`calloc`/`realloc`: TLSF over independent
   64 KB pools, with blocks over 256 KB on their own page runs and one empty
   pool cached. Host tests cover the allocator and the run records; the
   repeated `allocprobe` smoke profile checks that the kernel gets every page
   back. Note that user memory still comes from the frame allocator's fixed
   16 MB pool.

## Milestone 7 – Scheduler, SMP and threads

Goal: preemptive multitasking and threads on all cores. Locking follows Linux
2.0 (one big kernel lock); scheduling follows a hybrid of SMP and TempleOS's
explicit per-core placement.

**Design decisions**

- **One big kernel lock (BKL)**, taken on kernel entry from EL0 and released on
  return to EL0 and whenever a task sleeps. User programs run in parallel on all
  cores; the kernel itself is effectively single-threaded.
- **Fine-grained kernel locking is an explicit non-goal.** Smaller locks are
  added only where they are required for correctness (interrupt-shared data,
  below), not for scalability.
- **The kernel is not preemptible**: task switches happen on return to EL0 or
  when a task blocks. Kernel code never needs to defend against being switched
  out halfway through.
- **All device interrupts and all Circle code stay on core 0.** Circle's
  critical sections only mask interrupts on the local core, so it must never run
  elsewhere.
- **Interrupt handlers never take the BKL.** Data shared between handlers and
  the rest of the kernel (tty ring buffers, VirtIO input, timer state) is
  protected by small IRQ-safe spinlocks, because masking local interrupts on
  core N does not stop a handler running on core 0.
- **FP/SIMD state is context switched** (lazily, on first FP use after a
  switch). Today it is not saved at all because nothing in EL0 uses it.
- **`Forbid()`/`Permit()` syscalls** let a program take and release the BKL from
  EL0, so the user/developer can safely poke kernel structures while other cores
  run.
- Inter-processor interrupts are deferred: an idle core picks up new work on its
  next own timer tick.

**Scheduling model (hybrid, inspired by TempleOS)**

KoraOS is not a server OS balancing many similar requests. The expected
workload is a program that spawns a number of unbalanced sub-processes or
threads, with the user/developer deciding where work runs. So placement is
explicit and predictable rather than automatically balanced.

- **Per-core run queues, no migration.** A task stays for life on the core it
  was started on. Each run queue has its own IRQ-safe spinlock, because
  wakeups come from interrupt handlers on core 0 (which never take the BKL).
- **Placement happens once, at spawn.** By default a new task goes to the
  least-loaded core, so independent programs still spread across cores.
- **Explicit pinning.** `spawn` and `thread_create` take an optional core
  number; the shell exposes it too.
- **No work stealing** at first. If imbalance turns out to matter, "an idle core
  steals a runnable task" can be added later as a small, separate change.
- Unlike TempleOS, the kernel is not confined to core 0: any core can make
  syscalls under the BKL. Only interrupts and Circle are pinned to core 0.

**Threads**

The flat address space makes threads cheap: a thread is a task that shares its
process's image, heap and file table and has its own user and kernel stacks.
There is no page-table sharing to manage.

- A process lives until its last thread exits; its resources are freed then.
- Process exit marks the remaining threads killed; each one dies on its next
  kernel entry or timer tick on its own core, so no IPI is needed.
- On top of threads, `libk` provides a TempleOS-style job API: queue a function
  to a core and collect its result later.
- The same primitive backs `pthread` in the libc port (milestone 12).

**PRs**

1. **Task model rewrite** (done): 16 KB per-task kernel stacks with the EL0
   trap frame at the top, `cpu_switch` and `ret_to_user` replacing the nested
   `enter_user`/`kernel_return` model, the boot thread as task 0, and a
   minimal round-robin `schedule()`; spawn is create plus wait. Lazy FP/SIMD
   switching through CPACR traps. Circle is now built integer-only (one
   guarded edit in its `string.cpp`), and `tests/check-kernel-fp.py` keeps
   every kernel object free of FP/SIMD use, so interrupt handlers cannot
   clobber task registers. `fpprobe` and the `tasks` console command are
   tested in every smoke profile. Still cooperative and single-core.
2. **Preemption and sleeping** (done): the tick requests a reschedule, taken
   on every return to EL0 (10 ms round-robin slices). Wait queues; console
   `read` sleeps and is woken by the UART, VirtIO input and USB keyboard
   interrupts. `msleep(ms)` (syscall 16), `spawn` flags (`SPAWN_NOWAIT`) and
   `wait(pid, &code, flags)` (pid -1, `WNOHANG`); orphans are reaped by the
   kernel. The shell runs `cmd &` in the background and reports finished jobs
   at the next prompt. EL0 can read the virtual counter (`uptime_us()`).
3. **Locks and per-CPU data** (done): spinlocks on ARMv8.0 exclusives with
   IRQ-save variants and self-checks (recursion, foreign unlock, switching with
   a lock held), per-CPU data via `TPIDR_EL1`, the BKL (taken on entry from
   EL0, dropped while sleeping, never in interrupts) and `forbid()`/`permit()`
   (syscalls 17/18, Amiga semantics: no preemption while forbidden). The
   interrupt-masking audit converted `tty.c`, `virtio_input.c`, `irq.c`, the
   heap, the frame allocator and the scheduler to spinlocks; `mmu.c` keeps a
   local mask and now broadcasts its TLB invalidation; Circle stays
   single-core on core 0. A locked `printf` with a panic bypass, and
   `panic()`. Lock order and rules: `docs/locking.md`. Still one core.
4. **Secondary core bring-up** (done): PSCI `CPU_ON` on virt (`hvc` or `smc`
   from the device tree), the firmware spin table on Pi 3/Pi 4. Per-core stack,
   EL1 drop, shared MMU tables (MMU and caches on before touching any lock or
   shared data), vector table, idle task, per-core timer and per-core interrupt
   controller setup (GIC CPU interface; BCM local controller on Pi 3). Cores
   idle in `wfi`. Circle is configured single-core and stays on core 0. The
   boot line names the build's git commit. `docs/smp.md`. QEMU's hvf
   accelerator needs a GICv3 driver: a separate, virt-only step before PR 5.
5. **Per-core scheduling and placement**: per-core run queues, least-loaded
   placement at spawn, an optional core argument to `spawn`, shell syntax for
   pinning, and a `ps`-style command showing which core each task runs on. All
   device interrupts routed to core 0; idle cores pick up work on their tick.
6. **Threads and jobs**: `thread_create(fn, arg, core)`, thread exit and join,
   process lifetime and kill semantics as above; the `libk` job API on top.
7. **Userland and CI**: atomics in `libk`; demos for a job-parallel computation
   showing the speedup and for a program driving several unbalanced, pinned
   sub-processes; CI profiles on `virt -smp 4` and raspi3b (four cores),
   repeated stress runs, host unit tests for run-queue and placement logic.

**Risk**: PR 4 on real Pi 4 hardware is the least predictable step; QEMU does
not reproduce every hardware or memory-ordering behaviour.

## Milestone 8 – Volumes and namespace

AmigaDOS-style addressing of storage, instead of mounting everything under one
Unix root.

**Design decisions**

- **Device names** (`df0:`, `df1:`, and so on) use one slot sequence across
  ramdisk, VirtIO, SD and USB; one per mountable partition, not per physical disk. **Volume names** (for example `boot:`,
  `extras:`) come from the FAT volume label and follow the medium wherever it is
  attached. Both are matched case-insensitively; a device name always works,
  even when two media share a label.
- Typing `name:` alone in the shell changes the current directory to the root
  of that device or volume.
- Paths use `/` as the separator and `..` for the parent. A path starting with
  `/` is relative to the root of the **current volume** (like `\` on DOS
  drives), so POSIX-style code using `/bin/...` keeps working.
- **Assigns**: logical names such as `sys:` (the boot volume) and `c:` (the
  command directory, initially `sys:bin`). `spawn` searches `c:` instead of the
  hardcoded `/bin`.
- **Current directory**: each task inherits its parent's cwd. `getcwd` uses a
  unique volume label when it resolves back to the same root, otherwise the
  device name. The shell listing command is `volumes`. There is no synthetic
  unified tree (`/<volume>/...`).

**PRs**

1. **Block device registry and partitions**: multiple `blkdev_t` instances
   instead of one active device; MBR partition parsing; ramdisk and VirtIO
   register through it.
2. **Multi-volume FAT32 and resolver**: per-volume FAT32 state (allocated from
   the kernel heap), a volume registry with device and label names, a path
   resolver handling `name:path`, per-task current directory, `chdir`/`getcwd`
   syscalls.
3. **Shell and assigns**: `name:` as a directory change, `cd`, `pwd`, a command
   listing devices and volumes, assigns (`sys:`, `c:`), program lookup through
   `c:`.

## Milestone 9 – FAT32 write

Developed and tested on virt with VirtIO block, which already supports writes,
against scratch images. Never against a card that holds firmware until proven.

1. **Sector cache and data writes**: a small write-back sector cache, cluster
   allocation and freeing, writing and appending to existing files, truncate,
   FSInfo updates.
2. **Namespace operations**: create, unlink, `mkdir`, `rmdir`, rename; long
   filename and short-name alias generation.
3. **Sync and tools**: explicit sync, the volume dirty flag, write syscalls
   exposed to userland, `cp`/`rm`/`mkdir`/`mv` programs. CI runs `fsck.fat` (or
   `fsck_msdos`) on images after write tests, plus host unit tests on
   mtools-generated images.

## Milestone 10 – Storage drivers

1. **SD on Pi 3** (Arasan SDHCI) under the `blkdev_t` seam; evaluate reusing
   Circle's SD driver against writing one (license compatibility checked).
2. **SD on Pi 4** (EMMC2).
3. **USB mass storage** through Circle's existing mass storage class driver.

SD volumes mount read-only by default; writes are enabled explicitly until
milestone 9 has a track record on real cards.

## Milestone 11 – Hotplug

1. **USB plug and play**: drive Circle's plug-and-play updates from a kernel
   task on core 0; keyboards and mass storage can come and go. Volumes appear
   and disappear; open files on a removed volume return errors.
2. **HDMI hotplug**: detect connect/disconnect and re-initialise the
   framebuffer and console.

## Milestone 12 – libc port

Port a small C library (picolibc or newlib) onto the KoraOS syscalls, using the
milestone 6 allocator, milestone 7 FP context switching and threads (for
`pthread`), milestone 8 current
directory and milestone 9 writes. The kernel syscall ABI stays private; POSIX
shapes are provided by the library, not the kernel.

## QEMU virt development platform

An independent AArch64 virt target supports platform discovery, PL011/GICv2,
generic timer, ramfb, VirtIO MMIO block storage and keyboard. It preserves the
Pi hardware and raspi3b targets and runs the existing FAT32/EL0 environment.
See `virt-plan.md` for the completed PR bundles and validation. Automated
profiles cover embedded/external rootfs, graphics/serial, RAM sizes, EL2 entry,
and repeated allocation/process lifetime. It is the primary development target
for milestones 6 to 9 and 12.
