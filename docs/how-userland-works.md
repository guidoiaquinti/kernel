# How Userland Works

This explains what actually happens when KoraOS runs a program: how a binary
goes from a file on the filesystem to executing code, where it lives in memory,
how several programs coexist, and, importantly, the fact that **there is no
memory protection**.

See also [writing-userland-programs.md](writing-userland-programs.md) (author's
view), [syscalls.md](syscalls.md), and [filesystem.md](filesystem.md).

## From a name to running code

When something calls `spawn("ls", …)` (the shell) or the kernel starts
`/bin/init`, the same pipeline runs, in [`src/proc/task.c`](../src/proc/task.c):

1. **Resolve the name.** A bare name becomes `/bin/<name>`; an absolute path is
   used as-is.
2. **Read the file.** The program is read off the FAT32 filesystem into a
   temporary page-aligned buffer (`frame_alloc_pages`).
3. **Load the ELF.** [`elf_load`](../src/user/elf.c) parses the buffer and lays
   the program out in memory (details below). The temporary buffer is freed
   immediately afterwards, `elf_load` has copied everything it needs out of it.
4. **Set up the task.** A one-page user stack and a 16 KB kernel stack are
   allocated, a task-table slot and pid are assigned, and `argv` is copied onto
   the top of the user stack. The task's initial EL0 state (entry point,
   `SP_EL0` at the stack top, `x0`/`x1` = `argc`/`argv`) is written as a trap
   frame at the top of its kernel stack.
5. **Enter EL0.** The parent blocks until the child exits, and the scheduler
   switches to the child with `cpu_switch`
   ([`src/arch/entry.S`](../src/arch/entry.S)). A new task's first switch
   returns into `ret_to_user`, which restores that trap frame and `eret`s to
   the entry point.

The program runs until it returns from `main` (the entry stub then calls
`exit`) or calls `exit` directly, which marks it exited, wakes its parent and
switches away for good.

## Where a binary is placed

All dynamic memory comes from a single **physical page allocator**
([`src/mm/frame_alloc.c`](../src/mm/frame_alloc.c)): a fixed pool (16 MB) of
4 KB pages that begins just past the kernel image. Because the MMU uses a **flat
identity map** (see below), a physical page returned by the allocator is usable
as-is at the same address by both the kernel and EL0, there is no separate
"map it into the process" step.

User programs are built as **position-independent executables** (`ET_DYN`), so
they can load anywhere. `elf_load`:

- finds the address span of the program's `PT_LOAD` segments,
- allocates that many contiguous pages from the pool, call the base `region`,
- computes `bias = region - link_base` and copies each segment to
  `p_vaddr + bias` (the BSS tail is already zeroed by the allocator),
- applies the `R_AARCH64_RELATIVE` relocations (each fixed-up pointer becomes
  `bias + addend`), and
- reports the absolute entry point `e_entry + bias`.

So each spawned program gets its **own distinct region** in the pool. A task
holds three kinds of memory, all from the same pool:

| Region | Size | Notes |
|--------|------|-------|
| image  | a few pages | code + data + BSS, placed by `elf_load` |
| stack  | 1 page (4 KB) | `SP_EL0` starts at the top; `argv` lives here |
| page runs | any number | from `alloc_pages`, recorded per task; libk's `malloc` pools live here |

When a task is reaped, all of them are returned to the pool, including page
runs the program never freed.

## Several programs at once

Each task has its own kernel stack, with its EL0 registers saved in a trap
frame at the top and its kernel context (callee-saved registers and stack
pointer) saved by `cpu_switch` when it is switched out
([`src/proc/task.c`](../src/proc/task.c)). The boot thread, which runs
`kernel_main`, is task 0. Scheduling is **preemptive, on every core**:

- Each task runs on one core for life, chosen when it is created: the online
  core with the fewest tasks, or the one asked for (`SPAWN_CORE(n)`, the
  shell's `cmd @n`). Each core runs only its own tasks, so programs on
  different cores run in parallel; a core with nothing to do runs its idle task
  in `WFI` and picks up new work on its next tick (no inter-processor
  interrupts), so a wake-up from another core takes up to 10 ms to start.
- Each core's 100 Hz timer tick ends the running task's time slice; the switch
  to that core's next runnable task (round robin) happens on its way back to EL0. The kernel
  itself is not preemptible: a task in a syscall runs until it returns or
  blocks.
- A task **blocks** instead of spinning when it waits: for a child to exit
  (`spawn`, `wait`), for console input (`read` sleeps on a wait queue and is
  woken by the UART, VirtIO or USB keyboard interrupt), or in `msleep`. With
  nothing runnable the core waits for an interrupt (`wfi`).
- `spawn` is create plus wait: it creates the child and, unless asked for a
  background job (`SPAWN_NOWAIT`, the shell's `cmd &`), blocks the caller until
  the child has exited.
- A finished task becomes a **zombie** (its memory stays allocated so its exit
  code remains valid) until the parent reaps it with `wait`. If the parent
  exits first, its children become orphans that the kernel reaps itself.
- Up to `MAX_TASKS` (8) tasks can be live at once, including unreaped
  zombies.

Because of the identity map, **every task's image, stack, and heap are mapped
and addressable at the same time** (there is no address-space switch between
tasks). Distinct tasks simply occupy distinct regions of the one pool. `yield`
gives the rest of the time slice to another runnable task, if there is one.

**FP/SIMD registers are per task.** The kernel, the Circle USB code included,
is integer-only, so the registers only ever hold EL0 state, and they are
switched lazily ([`src/arch/fpsimd.c`](../src/arch/fpsimd.c)): the first
FP/SIMD instruction a task executes after a switch traps, the kernel saves the
previous owner's registers and loads this task's (zeroed on its first use), and
the instruction is retried. Tasks that never use FP never pay for it. The
debug console's `tasks` command lists the tasks with their state and kernel
stack high-water mark.

## Memory model, no protection, by design

The MMU ([`src/mm/mmu.c`](../src/mm/mmu.c)) installs a **flat identity map of the
low 4 GB** with 2 MB blocks: **virtual address == physical address**,
everywhere. The mappings distinguish code, ordinary RAM, coherent RAM and devices:

| Region | Covers | EL1 | EL0 | Executable |
|--------|--------|-----|-----|------------|
| code   | kernel/static text blocks, bounded by the target load address and `text_end` | read-only | read-only | yes |
| normal | the rest of RAM (the page pool, kernel data/stack/heap, the page tables, loaded programs) | read/write | **read/write** | at EL0 only |
| coherent | DMA pool/framebuffer (Normal non-cacheable) | read/write | read/write | yes |
| device | platform MMIO outside the selected RAM window | read/write | read/write | no |

On virt, RAM starts at `0x40000000`; the DTB supplies its size and device
locations. Code mappings cover the aligned kernel text range, while other RAM
holds loaded EL0 programs. Coherent DMA/framebuffer blocks are Normal
non-cacheable memory. The allocator still uses up to 16 MiB after the image,
clamped to RAM and excluding DTB/reserved regions. These platform differences
preserve the same flat, open memory model and process ABI.

The only hardware-enforced protections are:

- the **code region is read-only**, so nothing can scribble over kernel or
  static program text; and
- ordinary EL0-writable RAM pages are forced **PXN** (privileged-execute-never), which is why
  a program loaded into normal RAM runs at EL0 but the *kernel* cannot execute
  it, and why the kernel's own code lives in the separate read-only code block.

**Everything else is wide open, on purpose.** In particular:

> **There is no memory protection between programs, or between a program and the
> kernel.** Any EL0 program can read and write *all* of normal RAM: its own
> image, every other process's image/stack/heap, the kernel's data, stack, and
> heap, and even the MMU's own page tables. Device memory (peripherals) is
> reachable from EL0 too. There are no per-process address spaces, no guard
> pages, and no `NULL`-page trap.

This is not a limitation on the way to something stricter, **it is the model,
and it is meant to stay that way.** KoraOS is built in the spirit of the home
computers of the 1970s–90s: the Amiga, the Atari ST, the early Macintosh, the
DOS-era PC, machines with no MMU-enforced protection, where the whole system
was open to whoever was sitting in front of it. Nothing here is walled off from
you either. From a plain user program you can read and write kernel memory,
patch a running system call, poke a peripheral, or scribble over the page
tables, because that is exactly the kind of play the system is for.

The intended audience is the **user/developer** (to borrow Terry Davis'
phrase): someone who wants to *play* with the machine, understand it end to end,
and change it while it runs, not someone who needs a hardened OS to sandbox
untrusted apps. KoraOS is not trying to compete with Linux or a "professional"
OS; it deliberately sits in that older, opener space.

The flip side is the obvious one, and it is part of the fun: a single stray
pointer can take the whole system down. When that happens, you reboot and try
again, think of it as an Amiga guru meditation. Have fun, and keep a finger
near the reset button. :)
