# Tests

## Host unit tests (`tests/host/`)

Pure-logic kernel and shell parser code (code that does not touch hardware registers) is compiled
with the **host** compiler and run directly on your machine, under
AddressSanitizer and UndefinedBehaviorSanitizer. No cross toolchain, QEMU or
Raspberry Pi is needed, and a run takes a few seconds.

```bash
tests/run-host-tests.sh
```

Requires `clang` (the default `CC`; Apple's clang works), Python 3 and mtools
(`mformat`, `mmd`, `mcopy`) and dosfstools (`fsck.fat`) for real FAT32 fixtures. GCC is not supported:
it rejects the kernel's own `strlen` declaration under `-Werror`.

### Writing a test

- Add `tests/host/<name>_test.c` and a `run_test <name> <kernel sources...>` line
  in `tests/run-host-tests.sh`. Set `TEST_FLAGS` around the line for extra
  compiler flags (the libk malloc test uses it).
- Include `test.h` and the kernel header under test. Do **not** include system
  headers: the kernel's `common.h` defines its own fixed-width types, which
  clash with `<stdint.h>`. `test.h` declares the few libc functions tests need.
- Provide stubs for kernel services the code under test calls (for example the
  frame allocator), in the test file itself.
- Use `CHECK(cond, "message %d", ...)` and end the file with
  `TEST_MAIN(test_a, test_b, ...)`.

Current suites:

| Test | Covers |
|------|--------|
| `fat_namespace_test.c` | Create/exclusive-create, Greek/astral/maximal LFNs, strict UTF-8 and FAT names, short-alias collisions and lookup, directory growth/slot reuse, unlink/mkdir/rmdir/rename, object/cwd/assign guards, relative and qualified paths, cross-volume rejection, observed-allocation OOM sweeps, transient/persistent I/O recovery, raw VFAT/dot-link validation and fsck/mtools checks |
| `fat_write_test.c` | Existing-file writes on disposable 64 MiB mtools media: sector preservation, append, fragmented chains, first allocation, shared handle metadata, truncate and freed-cluster reuse, disk full, partition-view write/flush capabilities, FAT mirroring/active FAT/high bits, stale FSInfo, dirty eviction, retry after injected read/write/flush errors, cold remount, fsck and host readback |
| `libk_malloc_test.c` | `user/libk/malloc.c` (built with `LIBK_HOST_TEST`, which renames it `libk_malloc` and so on): alignment, neighbour separation, coalescing, pool growth and release, direct page-run blocks, bad and double frees, `realloc`/`calloc`, exhaustion, a 200 000-round stress with `kheap_info` consistency checks |
| `user_mem_test.c` | `src/proc/user_mem.c`: per-task page-run records behind `alloc_pages`/`free_pages`, foreign and double frees, reclaim on teardown, no leaks on failure |
| `cp_test.c` | Actual file-copy utility: partial reads/writes, exclusive destination preservation, partial-error output and durable-sync failure |
| `shell_input_test.c` | Real EL0 shell parser with renamed syscall stubs: fragmented reads, maximal complete lines, oversized-line draining, next-command preservation, 16/17-token boundary, terminated arguments, maximal assign target, quoted/escaped/empty tokens, literal operator provenance and malformed quoting |
| `filesystem_test.c` | `src/fs/fat32.c` and `src/fs/namespace.c` on mtools-made volumes: independent geometry/caches, root/BPB labels, duplicate labels/device collisions, relative paths, cwd isolation, retained handles, malformed geometry/chains/LFN, allocation and I/O errors, assign snapshots/replacement/removal/capacity, c-only command lookup and shadowed-label cwd, owned cwd/assign ancestor pins, busy reset and flush-failed reset preservation/retry |
| `blkdev_test.c` | `src/fs/blkdev.c`: bare FAT32 and mixed MBR discovery, hidden FAT32 types, invalid/range/overlap tables, partition-relative bounded read/write/flush, read-only and unsupported I/O, registry capacity |
| `spinlock_test.c` | `src/arch/spinlock.c`: one thread per simulated core; lock/trylock/irqsave bookkeeping, panics on recursive locking and foreign unlock (in forked children), and a contended counter with no lost updates or overlapping critical sections. Runs twice: on arm64 hosts the real `LDAXR`/`STXR` code, and the compiler-atomic fallback (`SPINLOCK_PORTABLE`) |
| `kmalloc_test.c` | `src/mm/kmalloc.c`, `src/mm/kmalloc_stress.c`: alignment, zeroing, cache-line separation, block reuse, slab release, page runs, over-aligned blocks, exhaustion, invalid and double frees, balanced IRQ masking, seeded stress |
| `term_test.c` | `src/video/term.c`: autowrap, scrollback and its view, scroll regions, insert/delete/erase, alternate screen, status replies, colours (16/256/24-bit, bce), UTF-8 and DEC line drawing, tabs, origin mode, cursor style/visibility, OSC, REP |

The scratch MBR fixture suite is independent of cross compilation and QEMU:

```bash
tests/run-mbr-image-tests.py
```

It checks aligned multi-partition layout, byte-for-byte source preservation,
invalid-input rejection and preservation of an existing output on failure.

`tests/run-filesystem-tests.sh` also runs independently. It creates disposable
2 MiB and 4 MiB mtools images with distinct content and labels, then makes
narrowly corrupted copies for chain and LFN tests. Fixtures live under
`build/host-tests/fs-fixtures`; the shared userfs is never used as writable media.

The write suite also runs independently:

```bash
tests/run-fat-write-tests.sh
```

It creates a pristine 64 MiB image, hashes it, and mutates disposable in-memory
copies. Three normal images cover valid, unknown and stale FSInfo metadata;
24 additional images cover interrupted allocation and shrink transport writes.
After recovery and cold remount, every exported image must pass `fsck.fat -n`
and exact mtools content readback. The source SHA256 must remain unchanged.
Forced disk-full BAD-cluster reservations and corrupt chains are separate
in-memory fault fixtures with no filesystem-integrity claim. All fixtures live
under `build/host-tests/write-fixtures`; the immutable shared userfs image is
never writable media.

## Integer-only kernel check

```bash
tests/check-kernel-fp.py build/debug/*/kernel.elf
```

Disassembles every object built for each kernel (Circle included) and fails on
any instruction naming an FP/SIMD register, or FPCR/FPSR, outside the
save/restore routines in `src/arch/fpsimd.S`. FP/SIMD registers belong to EL0
tasks and are switched lazily, so kernel code (above all interrupt handlers)
must not touch them; such a bug would corrupt user registers, typically only on
real hardware. Needs `llvm-objdump` (or `OBJDUMP=...`). CI runs it on the debug
and release builds.

The namespace suite also runs independently:

```bash
tests/run-fat-namespace-tests.sh
```

It creates a separate valid 64 MiB mtools source and checks its immutable SHA256.
Normal operations and 72 write/flush interruption cases are exported, plus one
consolidated allocation-recovery image per operation and four persistent-error
images. Every result must pass `fsck.fat -n`, exact mtools readback through short
aliases, FAT mirror equality and an independent raw directory parser checking
UTF-16, LFN ordinals/checksums/padding and dot links. An observed-allocation
sweep fails every heap allocation in each operation and checks previous names,
free-cluster counts and object ownership before retry. Directory extension,
read failures and failed rollback retention are covered too. A valid external
LFN resembling a generated short alias verifies that alias creation reserves
both naming forms. Full-disk BAD reservations remain isolated malformed fault
fixtures without an integrity claim. Images live under
`build/host-tests/namespace-fixtures`.

## Feature status file

```bash
tests/check-features.py
```

Validates `project_doc/features.yaml` (field names, statuses, dates, platforms,
PR numbers, and milestone numbers against `project_doc/roadmap.md`). Needs
PyYAML: `apt install python3-yaml`, or run it with
`uv run --no-project --with pyyaml python tests/check-features.py`.

## Build and shared-image regression tests

```bash
tests/run-build-tests.py
tests/run-userfs-tests.py
```

The CLI suite uses fake tool binaries to check repeatable/all target selection,
shared generation, Debug/Release isolation, first-target compilation database,
paths with spaces, deprecated aliases, validation before cleanup, prepared-image
consumption, failure-before-install gates and launcher argument forwarding. It
also exercises lock lifetime when the wrapper receives SIGTERM or SIGKILL.
It requires Python 3 and runs without cross-compiling or touching an SD volume.

The producer suite uses real Clang/CMake/mtools in an isolated temporary source
copy. It verifies no-op reuse, removed program registrations, header/fsroot
additions/removals, compiler-flag invalidation, Debug/Release userfs reuse and
atomic image failure that preserves the previous valid filesystem. CI runs it
in the userfs-producing job.

## QEMU smoke test

```bash
./build.sh --target qemu_raspi3b
tests/run-qemu-smoke.py --target qemu_raspi3b

./build.sh --target qemu_virt
tests/run-qemu-smoke.py --target qemu_virt --keyboard --disk build/userfs/aarch64/koraos.img --repeat 256
```

The test checks the boot line's build identifier (the git commit), the
boot-time kernel heap self-test, and that every core came online (`--smp N`
on virt, 1 to 4; raspi3b always has four), boots to the ELF shell,
runs filesystem/argv/exit-code checks plus an EL0 namespace probe (child cwd
inheritance and isolation, failed path handling, descriptors retained across
chdir, volume/assign enumeration and assign syscall updates), checks shell
`cd`/`pwd`/`volumes`/`assign`, current-directory `ls`, and excess-argument
rejection followed by a valid command, switches to the debug console,
verifies timer/UART IRQ progress, checks with `cpus` that every core is online
running its idle task (cores 1 and up) and that each core's own timer ticks,
runs `heaptest` (seeded kernel heap stress)
and checks that `heap` reports no leaks or bad frees, runs `fpprobe` (a parent
and child each fill all 32 vector registers and FPCR; the child must start from
zeroed registers and the parent's must survive), checks `tasks` (init and the shell
asleep, kernel stack high-water mark under three quarters), runs `schedprobe`
(`msleep(300)` takes at least 300 ms; a background job busy-looping without
syscalls must not stop the shell from running `echo`, so the tick preempts it;
the job is reported as done at a later prompt), runs `forbidprobe` (`forbid()`
nests and survives `msleep`; with a spinner competing, the task loses the CPU
for whole time slices normally but not while forbidden; a held `forbid()`
shows in `tasks` as the BKL holder, which is free again afterwards), and checks terminal
colors and graphics pixels using QEMU screenshots. Virt optionally injects
Shift/release, Backspace, Enter and scrollback through a VirtIO keyboard. UART
observes output. `--repeat N` runs `allocprobe` N times: page runs, a `malloc`
heap grown to about 1.5 MiB with churn and `realloc`, a nested child while it is
live, and deliberate leaks. The kernel heap's live count and the free page count
must be the same after the last run as after the first, so everything a reaped
task held, leaked or not, came back.

Use `--release` and `--build-dir` to match the build configuration. `--kernel`
and `--out` override the image and artifact locations. Defaults are the selected
`build/debug/<target>/kernel.img` and its `qemu-smoke/` directory. `--machine`
remains a deprecated alias for target selection. `--no-graphics` omits ramfb on
virt and tests serial fallback. `--disk-writable` negotiates writable test media;
file writes require a writable file descriptor. Use writable media only on
disposable scratch images.

An external disk may contain a bare FAT32 volume or primary MBR FAT32
partitions. The first supported partition supplies the boot root. To exercise
partition offsets using a separate scratch image:

```bash
tests/create-mbr-image.py --output build/mbr-root.img build/userfs/aarch64/koraos.img
tests/run-qemu-smoke.py --target qemu_virt --disk build/mbr-root.img --repeat 64
```

The `--multi-volume` profile additionally requires a second labeled partition:

```bash
tests/create-multivolume-image.py --output build/multi-root.img build/userfs/aarch64/koraos.img
tests/run-qemu-smoke.py --target qemu_virt --disk build/multi-root.img --multi-volume
```

The fixture copies the shared image, relabels its copies as BOOT and EXTRAS,
forces stale BPB labels, gives EXTRAS distinct README content and a private
`extrahello` command, and verifies the source SHA256 remains unchanged. The
profile assigns `c:` to EXTRAS while cwd remains on BOOT, executes `extrahello`
and `c:/hello`, then verifies missing or removed `c:` does not fall back to
BOOT `/bin`. It restores `c:` before subsequent checks.

The writable profile runs the complete VM-to-host integrity check:

```bash
./build.sh --target qemu_virt
tests/run-qemu-write.py
```

It creates a fresh 64 MiB FAT32 root from `fsroot` and registered prepared ELF
names, excluding stale ELF leftovers. The shared 2 MiB image is hashed and
never used as writable media. `writeprobe` covers creation, exclusive create,
read-only create, zero-byte permissions, large-length rejection, full fd-table
failure before mutation, data growth, partial-sector overwrite, shared append,
truncate, UTF-8 names, namespace and busy guards. Quoted CLI file utilities
exercise durable sync. After QEMU closes, `fsck.fat -n` and mtools verify exact
contents and removed paths; raw checks compare FAT mirrors and clean/error bits.
Logs and `verification.json` live in `build/debug/qemu_virt/qemu-write` by default.
`--out`, `--build-dir`, `--userfs-dir` and `--release` select other artifacts.

`--expect-root-failure --disk <zeroed-image>` requires a selected VirtIO disk,
failed mount and failed init load, so silent ramdisk fallback cannot pass.

## CI coverage

One producer job publishes `koraos.img` and user ELF symbols under a shared
AArch64 artifact. Consumers download it and use `--userfs-dir` without invoking
userland compilation or filesystem generation. The all-target job builds four
Debug kernels, stages both hardware Release payloads into a temporary directory,
creates a FAT32 SD image, and runs raspi3b smoke. Separate virt profiles cover
embedded graphics, 128 MiB external disk/keyboard, primary MBR boot, two labeled
partitions with namespace navigation and assign command lookup, 64 MiB
serial-only, EL2 entry and writable scratch-root integrity. UART/screenshots are uploaded on failure. Pure host
sanitizer suites stay
independent of the producer and kernel jobs.

QEMU cannot validate real Pi USB, HDMI, caches or boot firmware. Hardware testing
remains separate. Pi 3 USB IRQ counters include idle frame processing; compare
controller-specific behavior rather than interpreting them as keystroke counts.
