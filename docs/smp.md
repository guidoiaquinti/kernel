# Secondary cores

KoraOS brings up every core of the machine (up to four) and runs programs on
all of them. Device interrupts and the Circle USB stack stay on core 0 for
good (see [locking.md](locking.md)).

## Releasing the cores

Core 0 calls `smp_start_secondaries()` ([`src/arch/smp.c`](../src/arch/smp.c))
once its MMU, interrupt controller and system tick are up:

- **QEMU virt**: `PSCI CPU_ON` for each `cpu@N` node of the device tree, through
  the conduit its `/psci` node names: `hvc` without EL2, `smc` with
  `virtualization=on` (the `--el2` smoke profile).
- **Raspberry Pi 3 and 4** (and QEMU raspi3b): the firmware's ARM stub parks
  cores 1 to 3 in `WFE`, each polling its 64-bit slot of the spin table at
  `0xd8 + 8 * core`. Core 0 makes the firmware page writable, stores
  `secondary_entry` in the slot, writes the slot back to memory (the parked
  core reads it with its caches off) and sends `SEV`.

Core 0 waits up to a second for each core to report itself online and prints
`smp: cpu N online` (or that it did not come up), then `smp: N cores online`.

## A core's way up

A released core enters `secondary_entry` in [`src/boot.S`](../src/boot.S) at
EL2 (or EL3, or EL1 under PSCI without EL2), drops to EL1 through the same
code as core 0, takes its own 16 KB boot stack, traps FP/SIMD like core 0, and
calls `secondary_main(id)`:

1. `percpu_attach(id)`: only `TPIDR_EL1`.
2. `mmu_enable_this_core()`: the page tables core 0 built, with the MMU and
   caches on.
3. Then, and only then, everything else: its per-CPU entry, the exception
   vectors, an idle task (`idleN`), its side of the interrupt controller (the
   GIC CPU interface, or its routing register in the BCM2836 local
   controller), and its own generic-timer tick. It marks itself online and
   waits in `WFI` with interrupts enabled.

The order in step 3 is not a detail. Until its MMU is on, a core's memory
accesses bypass the caches while core 0's are cached:

- It must not take a spinlock: exclusives need Normal cacheable memory, and on
  real Cortex-A53/A72 cores the store-exclusive otherwise fails forever. The
  locks check this (`caches_on`) and panic instead of hanging.
- It must not write anything shared: a dirty line in another core's cache
  could later be written back over it. Before releasing a core, core 0 writes
  back and invalidates that core's stack and per-CPU entry, which are the only
  things it touches with its caches off; each `struct cpu` fills whole cache
  lines.

## Scheduling on every core

A task runs on one core for life, chosen when it is created
(`sched_place`, [`src/proc/placement.c`](../src/proc/placement.c)): the core
asked for (`SPAWN_CORE(n)`, the shell's `cmd args @n`), or else the online core
with the fewest live tasks, the lowest-numbered on a tie. There is no
migration and no work stealing; placement is explicit and predictable, in the
spirit of TempleOS.

Each core runs only its own tasks, round robin with its own 10 ms tick, so the
task table filtered by core is that core's run queue. One IRQ-safe scheduler
lock covers them all: wake-ups regularly cross cores (a keyboard interrupt on
core 0 wakes a shell on core 1, a child's exit wakes its parent elsewhere), and
with at most four cores serialized by the BKL, separate locks would only add
lock ordering. A task woken for another core sets that core's reschedule flag;
the core notices on its next interrupt, its own tick at the latest.

Every core has an idle task that runs when none of its tasks can (`idleN` in
`cpus`): it waits in `WFI` and looks for work after each interrupt. Core 0's
idle task has a stack of its own, because core 0's boot thread is task 0, which
blocks like any task. Since a task can exit on one core while its parent reaps
it on another, a task is marked `on_cpu` until the next task on its core has
left its stack; the reaper waits for that before freeing the kernel stack.

`ps` lists every task with its core; the debug console's `tasks` and `cpus`
show the kernel's view.

## Threads and jobs

A program can run on several cores at once with threads: a thread is a task
of the same process, with its own user and kernel stacks and FP/SIMD state,
sharing the image, heap, page runs, open files and cwd (with one flat address
space there are no page tables to share). `thread_create(fn, arg, core)`
places it exactly like `spawn` places a program, on the core asked for or the
least-loaded one, for life; `thread_join` collects its result. On top, libk's
TempleOS-style job API queues a function to a core and collects the result
later (`job_queue`, `job_result`), one thread per job.

A process lives until its last thread exits. `exit()` from any thread, or
`kill()`, marks the others killed; each dies on its own core at its next
kernel entry or return to EL0, so no inter-processor interrupt is needed (see
[locking.md](locking.md)). libk's `malloc` takes a mutex (`mutex_t`: spin, then
`yield()`), so every thread can share one heap.

## Timers and interrupts

Every core runs its own 100 Hz tick on its own generic timer, a per-core
source (a GIC PPI, banked per core, or the core's local timer routing on the Pi
3), which drives that core's scheduling. Core 0's tick is also the system
tick: the global tick count (and so `msleep` deadlines) and the Circle timer
hook. The debug console's `cpus` command shows
each core with its own tick count and what it runs.

## Trying it

```bash
./run-qemu.sh --target qemu_virt --smp 4
```

QEMU raspi3b always has four cores. The smoke test checks that every core comes
online and that its own timer ticks (`--smp 4` on virt; four CI profiles use it,
with GICv2 and GICv3, entering at EL1 (PSCI over `hvc`) or through EL2 (`smc`)).

On an Apple Silicon Mac, `--accel hvf` runs the cores on real hardware cores,
truly in parallel and with weak memory ordering:

```bash
./run-qemu.sh --target qemu_virt --accel hvf --smp 4
```

## GICv3 (virt)

QEMU's hvf accelerator only emulates a GICv3, so the virt kernel supports both
versions and picks the one the device tree describes
([`src/arch/irq_gic3.c`](../src/arch/irq_gic3.c)). The distributor uses
affinity routing and sends every shared interrupt to core 0; each core finds
its own redistributor frame by its MPIDR, wakes it and configures its timer
interrupt there; the CPU interface is the `ICC_*` system registers. Booting
through EL2, `boot.S` first lets EL1 use them (`ICC_SRE_EL2`).
