# KoraOS

[![CI](https://github.com/kora-os/kernel/actions/workflows/ci.yml/badge.svg)](https://github.com/kora-os/kernel/actions/workflows/ci.yml)

KoraOS is a bare-metal AArch64 kernel for Raspberry Pi 3/4 and QEMU
(`raspi3b` and `virt`). It runs an EL0 shell and utilities from a FAT32 user
filesystem with volume paths, assigns and file management tools. Writable
VirtIO media supports create, copy, append, truncate, rename and removal; the
embedded ramdisk remains read-only. Read the [Manifesto](MANIFESTO.md) for the project's direction.

## Quick start

Install LLVM/Clang, CMake 3.20+, Python 3, and mtools; install QEMU to emulate.

```bash
# One development target
./build.sh --target qemu_virt
./run-qemu.sh --target qemu_virt

# Show ramfb and use the virtual keyboard
KORA_QEMU_FB=1 ./run-qemu.sh --target qemu_virt

# Raspberry Pi 3 and Pi 4 under QEMU (raspi4b needs QEMU 9.0 or newer)
./build.sh --target qemu_raspi3b
./run-qemu.sh --target qemu_raspi3b
./build.sh --target qemu_raspi4b
./run-qemu.sh --target qemu_raspi4b

# All five targets (also the default when no target is specified)
./build.sh --target all
```

Targets are `qemu_raspi3b`, `qemu_raspi4b`, `qemu_virt`, `hw_raspi3b`, and
`hw_raspi4b`.
Repeat `--target` to select a subset. `--release` changes the kernel build
configuration; `--build-dir DIR` changes the build root.

Each kernel owns `build/<debug|release>/<target>/kernel.{elf,img,map}` and its
CMake cache. All targets consume the same AArch64 programs and FAT32 image
under `build/userfs/aarch64/`. The shared producer runs once before the selected
kernels. It publishes a new filesystem image only after successful generation.

## Disposable filesystem write test

Install dosfstools (`fsck.fat`) in addition to mtools, then run:

```bash
./build.sh --target qemu_virt
tests/run-qemu-write.py
```

The test creates a fresh 64 MiB scratch disk, writes through EL0 and verifies
it with fsck and mtools after QEMU exits. It preserves the shared userfs image.
See [Filesystem](docs/filesystem.md) for `cp`, `rm`, `mkdir`, `rmdir`, `mv`,
`sync`, quoting and volume paths. Write tests never use a Pi firmware card.

## Hardware installation

```bash
# Build both boards and copy a complete boot payload onto a mounted volume
./build.sh --target hw_raspi3b --target hw_raspi4b --release --install-to /Volumes/BOOT
```

Installation is explicit. It copies only selected hardware kernels, GPU boot
firmware, device trees, `config.txt`, and the firmware licence. The installed
names remain `kernel8-rpi3.img` and `kernel8-rpi4.img`; one card can serve both
boards. Eject the volume before inserting the card into the Pi.

To create a FAT32 boot image from already-built release kernels:

```bash
./create-sd-image.sh --target all --release --output build/koraos-boot.img
```

The firmware is loaded before KoraOS and is not linked into it; see
[firmware/README.md](firmware/README.md).

## Shared userfs and tooling

```bash
./build.sh --userfs-only
./build.sh --target qemu_virt --userfs-dir build/userfs/aarch64
```

`--userfs-dir` explicitly consumes a prepared image without rebuilding it.
Normal builds update the shared producer automatically. Kernel Debug and Release
share userfs because its current AArch64 PIE flags are identical in both modes.

`build/compile_commands.json` reflects the first selected kernel target for
clangd. For a multi-target build, put the development target first. With a custom
build root, configure clangd to use that root's compilation database.

Old `--qemu`, `--virt`, and `--hw` flags are deprecated aliases; their outputs
also use the new directory layout. `RPI_VERSION` is needed only by the old `--hw`
alias. Target selection and installation no longer need environment variables.

## Documentation

- [Developer guide](docs/developer-guide.md): toolchain, targets, QEMU and hardware.
- [Filesystem](docs/filesystem.md): shared userfs producer, FAT32 and block backends.
- [Userland model](docs/how-userland-works.md): ELF loading, allocation and processes.
- [System calls](docs/syscalls.md): syscall ABI.
- [Writing programs](docs/writing-userland-programs.md): adding userland programs.
- [Tests](tests/README.md): host, build CLI and QEMU verification.

## License

KoraOS is GPL-3.0-or-later. See [LICENSE](LICENSE) and [CREDITS.md](CREDITS.md).
