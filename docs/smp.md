# Secondary cores

KoraOS brings up every core of the machine (up to four). For now the other
cores only idle with their own timer ticking; Milestone 7 PR 5 gives them run
queues. Device interrupts and the Circle USB stack stay on core 0 for good
(see [locking.md](locking.md)).

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

## Timers and interrupts

Every core runs its own 100 Hz tick on its own generic timer, a per-core
source (a GIC PPI, banked per core, or the core's local timer routing on the Pi
3). Only core 0's tick is the system tick: the global tick count, the
scheduler and the Circle timer hook. The debug console's `cpus` command shows
each core with its own tick count and what it runs.

## Trying it

```bash
./run-qemu.sh --target qemu_virt --smp 4
```

QEMU raspi3b always has four cores. The smoke test checks that every core comes
online and that its own timer ticks (`--smp 4` on virt; two CI profiles use it,
one of them entering through EL2, so PSCI over `smc`).
