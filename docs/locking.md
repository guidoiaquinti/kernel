# Locking

KoraOS follows Linux 2.0: one **big kernel lock** (BKL) serializes everything
tasks do inside the kernel, and a few small **spinlocks** protect the data that
interrupt handlers share with the rest of the kernel. Fine-grained kernel
locking is a non-goal (see `project_doc/roadmap.md`, Milestone 7); a lock is
added only where correctness needs it.

## Spinlocks

[`arch/spinlock.h`](../include/arch/spinlock.h) implements spinlocks with
ARMv8.0 exclusives (`LDAXR`/`STXR`; the Cortex-A53 has no LSE atomics), waiting
in `WFE` and releasing with `STLR`, which wakes the waiters.

- Data an interrupt handler also touches is locked with `spin_lock_irqsave` /
  `spin_unlock_irqrestore`. Masking IRQs keeps a handler on the same core from
  spinning on a lock its own core holds; the lock keeps other cores out.
  Masking alone is never mutual exclusion once several cores run.
- Never block or switch tasks while holding a spinlock.
- The locks check themselves: taking a lock this core already holds, releasing
  one this core does not hold, and switching tasks with a spinlock held all
  panic with the lock's name instead of hanging.

Per-CPU data ([`arch/percpu.h`](../include/arch/percpu.h)) lives in a
`struct cpu` that `TPIDR_EL1` points at: the running task, the
reschedule flag, the FP/SIMD owner and the held-lock count. Only the owning core
writes its entry, so it needs no lock.

## The big kernel lock

- A task takes the BKL on every kernel entry from EL0 (syscalls and other
  synchronous exceptions, in `src/arch/vectors.S`) and releases it on the way
  back. While waiting for it, the core keeps interrupts open.
- A task that blocks (in `read`, `wait`, `msleep`, or a foreground `spawn`)
  drops the BKL while it sleeps and takes it back when it runs again, so a
  sleeping task never keeps other tasks out of the kernel.
- **Interrupt handlers never take the BKL.** They only use the small locks
  below, which is why that data needs them.
- The boot thread (`kernel_main`, task 0) holds the BKL while it brings the
  system up, and releases it for good once it only serves the serial line.
- Kernel code is not preemptible: a task in the kernel runs until it returns
  to EL0 or blocks. Preemption happens only on the way back to EL0.

BKL-protected, among others: the process and task tables and parent links,
fd tables, thread creation, exit and kill, the filesystem and volume state,
the screen (only tasks draw it), and the page tables (`mmu_map_coherent` only
masks local IRQs and uses broadcast TLB maintenance).

### Killing threads without interrupting cores

`exit()` and `kill()` end a whole process, but its other threads may be
running on other cores at that moment. Under the BKL, the killer only marks
them `killed` (and wakes those that are blocked, under `sched`); it never
touches another core. Each thread checks its own flag on its own core: on
kernel entry (`handle_sync_el0`, holding the BKL) and on every way back to EL0
(`sched_preempt_check`, after a syscall, its tick, or being switched in), where
it takes the BKL and exits. A woken thread's blocking loop (`read`, `wait`,
`thread_join`; `msleep` simply returns) sees `task_killed()` and returns
through the syscall normally, so it dies holding no lock and no buffer. A
thread running only EL0 code dies on its core's next tick, within 10 ms.

### forbid() and permit()

`forbid()` (syscall 17) lets a program keep the BKL after returning to EL0, so
it can read or change kernel structures while no other core can be inside the
kernel; `permit()` (18) gives it back. As on AmigaOS, a forbidden task is also
not preempted on its core (interrupts still run), calls nest, blocking breaks
the forbid only while the task sleeps, and exiting ends it. The debug console's
`tasks` command marks forbidden tasks and shows who holds the BKL.

## Locks and their order

Take locks only in this order (a lock may be skipped, never taken backwards):

1. **BKL**
2. `serial` (`src/tty.c`): UART draining and console routing; debug console
   commands run under it
3. `virtio keyboard` (`src/drivers/virtio_input.c`): queue setup against its
   interrupt handler
4. `tty input` (`src/tty.c`): the console input queue, scrollback requests,
   and the condition readers sleep on
5. `sched` (`src/proc/task.c`): task states, wait queues, sleep deadlines;
   held across a context switch and released by the task switched to
6. `heap` (`src/mm/kmalloc.c`), then `frames` (`src/mm/frame_alloc.c`)
7. `irq table` (`src/arch/irq.c`): handler registration against dispatch
8. `printf` (`src/lib/panic.c`): whole `printf` calls; bypassed while panicking

A task blocks by marking itself blocked under `sched` before it drops the BKL,
and a wait queue sleeper is queued before it releases the condition's lock, so
no wake-up can fall between checking a condition and sleeping.

## Circle

The vendored Circle USB stack is built single-core (`ARM_ALLOW_MULTI_CORE`
off): its own critical sections only mask IRQs on the local core. That is
enough because Circle runs only on core 0 (its interrupts are routed there,
and nothing calls it from other cores), and everything it shares with the rest
of the kernel (the heap, the frame allocator, the input queue, the IRQ table)
is protected by the locks above.
