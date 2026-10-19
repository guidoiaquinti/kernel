# Developer Guide

This guide captures the current build and run workflow for KoraOS. It focuses on the LLVM/Clang toolchain, the CMake-based build system, and how to exercise the kernel under QEMU and on real Raspberry Pi hardware.

## Toolchain Overview

KoraOS is built with the LLVM toolchain while targeting `aarch64-none-elf`. The CMake build system generates `compile_commands.json`, which keeps clangd and other tooling in sync with the cross-compilation flags.

### Required Packages

- LLVM/Clang (including `clang`, `ld.lld`, and `llvm-objcopy`)
- CMake 3.20 or newer
- Python 3 (shared-build coordination and regression tooling)
- [mtools](https://www.gnu.org/software/mtools/) (`mformat`, `mcopy`), used to
  build the embedded FAT32 filesystem image; the build fails without it
- QEMU (only for virtualization workflows)
- Optional: GNU Make (the legacy Makefile is still functional)

### Installing the Toolchain

macOS (Homebrew):

```bash
brew install llvm cmake qemu mtools
echo 'export PATH="/opt/homebrew/opt/llvm/bin:$PATH"' >> ~/.zprofile
```

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install clang lld llvm llvm-objcopy cmake qemu-system-aarch64 mtools
```

After installation, confirm the tools are available:

```bash
clang --version
ld.lld --version
llvm-objcopy --version
```

### Generating `compile_commands.json`

Run `./build.sh` at least once. CMake writes `build/compile_commands.json`, which clangd automatically discovers via the repository's `.clangd` configuration.

## Building and selecting targets

```bash
./build.sh --target qemu_raspi3b
./build.sh --target qemu_virt
./build.sh --target hw_raspi3b --release
./build.sh --target hw_raspi4b --release
./build.sh --target hw_raspi3b --target hw_raspi4b --release
./build.sh --target all
```

No target argument means all four. Repeated selections are deduplicated in
first-seen order. `--debug` (default) or `--release` applies to selected kernel
configurations. Each has its own cache and `kernel.elf`, `kernel.img`, and
`kernel.map` in `build/<debug|release>/<target>/`; `--build-dir` sets the root.
The shared userfs producer owns `build/userfs/aarch64/user/*.elf` and
`build/userfs/aarch64/koraos.img`, independent of the selected board and kernel
configuration. It keeps the current AArch64/O2/PIE userland flags.

`--clean` rebuilds selected configurations and the owned shared userfs, preserving
unselected configurations and explicitly supplied userfs. Invalid arguments are
rejected before cleanup. `clean` removes selected generated configurations
without rebuilding. The shared producer is serialized against another build
invocation and publishes its image atomically, keeping the previous valid image
if generation fails. Avoid running kernel consumers while replacing their image
through an unrelated manual command.

```bash
# Build the shared artifact alone; consume it explicitly (as CI does)
./build.sh --userfs-only
./build.sh --target qemu_virt --userfs-dir build/userfs/aarch64
```

A supplied `--userfs-dir` is a prepared artifact: the caller controls its contents
and freshness. Its `koraos.img` must exist and be nonempty. CI produces it from the
same checkout and downloads it into each consumer job.

The first selected kernel's compilation database is copied to
`build/compile_commands.json`, matching `.clangd`. A custom build root needs a
matching clangd setting. Select the development target first for multi-target
builds.

## Running QEMU

```bash
./run-qemu.sh --target qemu_raspi3b
./run-qemu.sh --target qemu_virt
KORA_QEMU_FB=1 ./run-qemu.sh --target qemu_virt
./run-qemu.sh --target qemu_virt --release --build-dir my-build -s -S
./run-qemu.sh --target qemu_virt --smp 4
```

`--smp N` (or `KORA_QEMU_SMP`) gives virt 1 to 4 cores (default 1); QEMU
raspi3b always has four. See [smp.md](smp.md).

The launcher uses the matching configuration's `kernel.img` and forwards extra
QEMU arguments. Ctrl-A X quits. `KORA_QEMU_DISPLAY` overrides the native display
choice (cocoa on macOS, gtk on Linux). The build and launcher share target names,
build roots and configuration flags.

## Raspberry Pi deployment

```bash
./build.sh --target hw_raspi3b --target hw_raspi4b --release --install-to /Volumes/BOOT
```

All selected builds must succeed before installation starts. Only selected
hardware targets are installed, with firmware, DTBs, config and licence. Existing
boot filenames remain `kernel8-rpi3.img` and `kernel8-rpi4.img`. Building alone
does not copy anything to an external volume. Low-level hardware configurations
retain `install_hw`, using their cached `BOOTMNT` destination.

```bash
# Package already-built hardware targets without mounting or root privileges
./create-sd-image.sh --target all --release --output build/koraos-boot.img
```

### Verifying the Image

List the files on the boot volume to ensure `kernel8-hw.img` is present, eject the volume safely, and boot the Raspberry Pi. The kernel prints `K` on the serial console, followed by its boot log.

On hardware the serial console and the user console are separate. The UART carries the kernel's diagnostic log (`printf`, `console_log`); the shell runs on the HDMI screen and reads the USB keyboard: through the DWC2 controller on the Pi 3, and through the VL805 USB 3 (xHCI) controller behind PCIe on the Pi 4, on any of its four type-A ports (USB 2 or USB 3), directly or through a hub.

USB limitations: devices are enumerated once at boot, so plug the keyboard (or a hub with the keyboard) in before powering on; there is no hot-plug yet. On the Pi 4 only the four type-A ports are supported (the VL805 xHCI controller); the USB-C port is a different controller (DWC2) that the Pi 4 build does not include, and it normally powers the board anyway. On the Pi 3 a keyboard behind an extra hub does not work yet: low/full-speed devices behind a High Speed hub need split transactions, which the DWC2 has to schedule in software (the Pi 4's xHCI does this in hardware). Keyboard auto-repeat is not enabled yet.

Typing on the serial line talks to the kernel debug console (`koraos> `, `src/console.c`), which runs alongside the shell: it is fed from the UART interrupt and from the shell's idle loop, not a task of its own, so its commands must not block. Press **Ctrl-T** to hand the serial line to the screen terminal as a fallback keyboard (what you type is then echoed on the screen, not on the serial line), and Ctrl-T again to return. Besides `help` and `version`, `irqs` shows the interrupt controller in use (the legacy BCM controller on the Pi 3, the GIC-400 on the Pi 4), the system tick count next to the uptime (about 100 ticks per second when interrupts work), and how often each connected IRQ has fired.

Under QEMU, which has no USB keyboard, the serial line starts on the screen terminal and the shell is mirrored to the UART as before; Ctrl-T reaches the debug console. The keyboard layout defaults to US; pick another of Circle's keymaps at configure time with `-DKORAOS_KEYMAP=IT` (or UK, DE, FR, ES, DV).

The screen terminal understands the xterm escape sequences that the `xterm-256color` terminfo entry uses: cursor movement and addressing, erase/insert/delete, scroll regions, SGR attributes with 16, 256 and 24-bit colour, the alternate screen, save/restore cursor, tab stops, DEC line drawing (and UTF-8 box characters), and the status/size reports. `termdemo` shows most of it. **Shift+PgUp / Shift+PgDn** scroll back through the history (`TERM_SCROLLBACK_LINES`, default 1000); new output returns to the live screen. The cursor is a block by default; set `TERM_DEFAULT_CURSOR` in `include/video/term.h` to `TERM_CURSOR_UNDERLINE` or `TERM_CURSOR_BAR` (programs can also change it with `ESC [ n SP q`).

## Tests

Pure-logic kernel code has host-side unit tests that run on your machine under AddressSanitizer/UndefinedBehaviorSanitizer, with no QEMU or hardware:

```bash
tests/run-host-tests.sh
```

A QEMU smoke test boots the kernel on `raspi3b` and drives the shell over serial, checking the output and framebuffer screenshots:

```bash
./build.sh --target qemu_raspi3b
tests/run-qemu-smoke.py --target qemu_raspi3b
```

See `tests/README.md` for what each covers and how to add tests.

## Further Documentation

- [filesystem.md](filesystem.md) – FAT32 volumes, reads/writes, the ramdisk and disposable write testing.
- [how-userland-works.md](how-userland-works.md) – what happens when a program is loaded, where it lives in memory, how processes coexist, and the (deliberate) lack of memory protection.
- [syscalls.md](syscalls.md) – the full system-call ABI.
- [writing-userland-programs.md](writing-userland-programs.md) – how to write, build, and run a userland program (no compiler or libc on the device yet).


## QEMU virt development profile

```bash
./build.sh --target qemu_virt
KORA_QEMU_FB=1 ./run-qemu.sh --target qemu_virt
KORA_QEMU_DISK=build/userfs/aarch64/koraos.img ./run-qemu.sh --target qemu_virt
```

The supported profile is one cortex-a72 CPU, GICv2 and TCG, with 256 MiB RAM
below 4 GiB. `KORA_QEMU_RAM=128M` changes RAM. The kernel discovers RAM/device
locations from QEMU's DTB; use raw `kernel.img` for boot and `kernel.elf` for
symbols. Pi GPIO, mailbox and Circle USB are excluded from virt.

ramfb provides a 1024x768 XRGB8888 screen and `fb_info`; `KORA_QEMU_RAMFB=0`
omits it for serial fallback. The VirtIO keyboard uses a US keymap with
Shift/Ctrl/Caps, editing/navigation and Shift+PgUp/PgDn scrollback.
`KORA_QEMU_KEYBOARD=0` omits it. UART mirrors shell output and Ctrl-T switches
to the debug console. Pi USB layouts still use the Circle keymap setting.

An external disk uses modern VirtIO MMIO and defaults to read-only media.
`KORA_QEMU_DISK_READONLY=off` enables FAT32 file writes on a supplied disposable
image; the embedded ramdisk stays read-only. Never enable writes on the shared
`build/userfs/aarch64/koraos.img` artifact or a card containing Pi firmware. A missing disk uses embedded userfs;
a configured broken disk does not silently fall back. See `filesystem.md`.

For an end-to-end write test, install dosfstools (`fsck.fat`) alongside mtools:

```bash
./build.sh --target qemu_virt
tests/run-qemu-write.py
```

This creates a fresh 64 MiB scratch root, runs EL0 writes and quoted file tools,
then verifies it with fsck and exact mtools readback after the VM closes. The
immutable shared image hash is checked before and after the run. `cp` creates
new destinations only; `mv` does not overwrite or move across volumes. All
mutating tools sync before reporting success. Use the shell's `sync` command
to flush changes made by other programs. Names use UTF-8, while interactive
console input currently accepts ASCII; quotes handle spaces in ASCII paths.

CI builds shared userfs once and passes its artifact to kernel jobs. It builds
all four targets, both hardware release targets, a complete SD payload, and
retains host sanitizer/CLI tests and raspi3b smoke. Independent virt profiles
cover embedded graphics, external disk/keyboard at 128 MiB, serial-only 64 MiB,
EL2 entry, writable scratch-root integrity, and repeated EL0 page/malloc allocation and nested-process lifetime. Logs and
screenshots are retained on failure. QEMU does not validate real Pi USB, HDMI,
cache behavior or firmware. The current virt profile has no SMP, GICv3, PCI,
networking, audio or VirtIO GPU; scheduling remains cooperative.

## Migration and direct CMake

`--qemu`, `--virt`, `--hw` and `--variant` remain deprecated aliases. Hardware
aliases use `RPI_VERSION`, while canonical names select the board explicitly.
Aliases produce the new directory layout too. `BOOTMNT` does not implicitly
install; use `--install-to`. Direct legacy CMake variant options remain available,
but normal development should use the orchestrating script.

Modern kernel CMake configurations receive `KORAOS_TARGET` and
`KORAOS_USERFS_DIR`; they only consume the prepared image. The standalone producer
is `cmake/userfs`. Register user programs in `cmake/userfs.cmake`.
Cross-target static libraries use LLVM ar/ranlib. Use a fresh build directory
when changing archive tools in an existing CMake cache.
