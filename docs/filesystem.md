# Filesystem

KoraOS supports **FAT32 reads, writes and namespace operations through EL0
syscalls and file utilities**. The kernel uses it to find and load userland programs: the kernel boots `/bin/init`, which starts
`/bin/shell`, which loads `/bin/ls`, `/bin/cat`, and the rest, all as
independent binaries read from the filesystem, not embedded in the kernel image.

## Where the disk comes from

A kernel built with `--ramdisk SIZE` carries a **ramdisk**: a FAT32 volume
with the user programs and files, laid out in RAM at boot and read-write (see
below). Otherwise the system comes from a disk: the SD card or a USB drive on a
Pi, a VirtIO MMIO disk on QEMU virt (which is the root even with a ramdisk).
All backends register together; FAT32 mounts every valid partition and uses
the boot partition as the initial working volume.

On the Pi 3 and Pi 4 (and QEMU raspi3b) the **SD card** registers too, through
[`src/drivers/sdhci.c`](../src/drivers/sdhci.c): a polled SDHCI driver. On the
Pi 3 it drives the Arasan controller, which takes over the card's pins from
the firmware's SDHOST controller; on the Pi 4 it drives EMMC2, which the slot
is wired to. Standard and high-capacity cards work, on a 4-bit bus at
25 MHz, with multi-block transfers. The card's FAT32 partitions become volumes
next to the ramdisk, if any; without one, a card with `/bin/init` is the boot
volume. The card is read-only unless
the kernel is built with `--storage-writable`. Transfers run in the calling
task under the BKL with interrupts masked.

**USB drives** plugged in at boot register too, through Circle's bulk-only
mass storage driver behind [`src/circle/usb_storage.cpp`](../src/circle/usb_storage.cpp):
on the Pi 3's DWC2 and the Pi 4's xHCI ports, and in QEMU raspi3b. Their FAT32
partitions become volumes like the card's, read-only unless the kernel is
built with `--storage-writable`. Transfers run in the calling task under the
BKL, on any core; the task opens interrupts on its core while it waits, since
USB completions arrive as interrupts on core 0. Single-sector reads are served
from a 16 KB read-ahead window per drive.

```
create-fs-image.sh ──(mtools)──▶ build/userfs/aarch64/koraos.img ──▶ create-ramdisk-image.py ──(.incbin)──▶ kernel image
       ▲                                     │                       (--ramdisk SIZE)            _koraos_ramdisk_start/_end
   fsroot/ + build/userfs/aarch64/user/*.elf  └──(install, create-sd-image.sh)──▶ SD card boot partition
```

- [`create-fs-image.sh`](../create-fs-image.sh) formats a bare FAT32 volume
  (BPB at LBA 0, no MBR/partition table) and
  populates it. It requires **mtools** (`mformat`, `mcopy`); see the
  [developer guide](developer-guide.md).
- Everything under [`fsroot/`](../fsroot) is mirrored into the image root.
- Each built user program (`build/userfs/aarch64/user/<name>.elf`) is installed as
  `/bin/<name>` (the `.elf` suffix is stripped, so programs are spawned by bare
  name).
- The standalone userfs producer (`cmake/userfs`) runs the script once. The
  image is rebuilt whenever anything in `fsroot/` or any user program changes,
  and is never written to.
- A kernel built with `./build.sh --ramdisk SIZE` embeds a **ramdisk**:
  [`create-ramdisk-image.py`](../create-ramdisk-image.py) formats a FAT32
  volume of SIZE, copies the userfs contents in (keeping their directory
  order), and keeps only its non-zero 4 KiB runs, so even a `1G` ramdisk adds
  a few hundred KB to the kernel image. At boot
  [`src/fs/ramdisk.c`](../src/fs/ramdisk.c) finds free RAM past the page pool
  (on a Pi from the firmware's ARM memory size and board revision, so a Pi 4
  with 2 GB or more puts a large ramdisk above its first gigabyte; on virt from
  the device tree), zeroes it and copies the runs in. The ramdisk is
  read-write; its changes last until the machine stops.
- Without a ramdisk the boot volume is the first registered volume with a
  file at `/bin/init` (SD card, then USB drives; on virt a configured VirtIO
  disk is the root, as with a ramdisk). The hardware install and
  `create-sd-image.sh` copy the userfs contents onto the card's boot partition
  for that.

## Layers

```
  task_spawn()  ──▶ resolves program paths, hands bytes to elf_load() (src/proc/task.c)
  file syscalls ──▶ open / read / write / sync / namespace           (src/sys/syscall.c)
  namespace     ──▶ device / volume names and per-task cwd       (src/fs/namespace.c)
  ─────────────────────────────────────────────────────────────────────────────
  FAT32 driver  ──▶ mount, path walk, FAT chains, LFN→UTF-8, read/write  (src/fs/fat32.c)
  ─────────────────────────────────────────────────────────────────────────────
  block device  ──▶ blk_read(lba, count, buf), 512-byte sectors    (src/fs/blkdev.c)
  ─────────────────────────────────────────────────────────────────────────────
  ramdisk, SD, USB, VirtIO ──▶ backends (src/fs/ramdisk.c, src/drivers/, src/circle/)
```

Each layer has a clean seam:

- **Block device** ([`include/fs/blkdev.h`](../include/fs/blkdev.h)), a
  `blkdev_t` with read/write/flush operations over 512-byte sectors. A registry
  holds up to eight physical backends and four FAT32 primary partitions per
  backend. Ramdisk, VirtIO, SD and USB drives register with an explicit backend
  type. Each partition exposes
  a bounded `blkdev_t` view whose LBA zero is its first sector. Bounds checks
  happen before transport I/O, so reads and writes cannot cross a partition.
- **FAT32** ([`include/fs/fat32.h`](../include/fs/fat32.h)), per-volume state
  allocated from the kernel heap, independent geometry and sector
  caches, directory traversal, file reads and writes, and stat. Each open handle retains
  its volume, so changing cwd does not redirect existing descriptors. No VFS.
- **File syscalls + per-task fd table**, see [syscalls.md](syscalls.md).

## FAT32 details

- **Mount** parses the BPB at sector 0 (bytes/sector, which must be 512,
  sectors/cluster, reserved sectors, number of FATs, `FATSz32`, `RootClus`) and
  validates geometry against the partition bounds and FAT capacity before
  caching it. Corrupt or looping chains report errors instead of hanging.
- **Cluster chains** use a bounded write-back sector cache. Dirty cache victims
  remain available after an I/O error so an explicit sync can retry them.
- **Long filenames (LFN/VFAT)** are decoded from their on-disk **UTF-16** to
  **UTF-8**, including surrogate pairs. UTF-8 is used everywhere names cross the
  syscall boundary, it is lossless versus the UTF-16 source and keeps the whole
  byte-string / C-string userland (argv, the shell, `write`) working unchanged.
  Malformed, incomplete or checksum-invalid LFN sequences fall back to their
  short-name entry.
  Names can be up to 255 UTF-16 code units, i.e. up to 765 UTF-8 bytes.
- **8.3 short names** are an OEM code page, which the driver does not decode:
  non-ASCII short-name bytes become `?`. Names that genuinely need non-ASCII
  characters carry an LFN, which is decoded fully.
- **`mformat -F`** forces a FAT32 BPB even for a small volume (the 2 MiB
  shared userfs, a 16 MiB ramdisk), below the usual 65525-cluster minimum. The
  driver only reads the BPB, so this is fine.

## Existing-file writes and durability

`fat32_open_flags` accepts read-only, write-only or read/write access, optional
append and truncate-on-open flags. Ordinary `fat32_open` remains read-only.
Create opens an existing file without truncating it unless truncate is requested;
exclusive create fails when a long name or its short alias already exists.
Create can return a read-only handle on writable media; exclusive access
requires create. Truncate and append require writable access. A read-only
medium or a FAT read-only file rejects mutation. Writable transports must
support both writes and flush;
missing durable flush support fails before media changes.

Open handles share reference-counted metadata identified by the short directory
entry's sector and offset. Their byte cursors remain independent. Append selects
the shared EOF for each write, so a reader opened earlier sees later extensions.
`fat32_close` releases its handle reference. Current directories and assigns
retain their directory and ancestor references too, so namespace changes can
reject removal of an in-use path. `fs_cwd_copy` inherits owned cwd references;
`fs_cwd_release` releases them when a task exits. Resolver results are temporary
path snapshots. A volume with live references cannot unmount. Namespace reset
checks external owners and syncs every volume before releasing assigns and
mounts; busy or failed sync leaves the namespace available for retry. Truncate preserves cursor positions. A cursor beyond a new, smaller
EOF must seek back before writing because sparse holes are unsupported.

Allocation checks actual FAT entries rather than trusting FSInfo counts or
hints, zeroes new clusters before publishing links, preserves reserved FAT high
bits, and follows BPB ExtFlags for active FAT selection or mirroring. Shrink
validates the full chain before reclaiming any clusters. Directory entries track
both the new size and first cluster. FSInfo counts and hints are refreshed during
sync. Exhaustion after progress returns a short write; exhaustion before progress
returns `FS_ERR_NOSPC`.

`fat32_sync_volume` flushes cached data, FAT copies, directory metadata and
FSInfo through the block transport. Mutation clears the FAT clean bit before
writing; only a successful sync sets it again. Read, write and flush failures
reach the caller. Interrupted mirrored updates, unpublished allocations and
detached shrink tails remain recoverable for a later retry. This is a serialized
write path under the kernel lock, with no filesystem blocking waits. It is not a
journaled filesystem and does not promise power-loss atomicity.

Write tests use disposable 64 MiB mtools images with valid FAT32 cluster counts,
never the shared producer artifact or a firmware card. The host sanitizer suite
checks cold remounts and injected transport failures; independent `fsck.fat -n`
and mtools readback verify the resulting images:

```bash
tests/run-fat-write-tests.sh
```

## Writable syscalls and file tools

`open` accepts `O_RDONLY`, `O_WRONLY` or `O_RDWR`, combined with `O_CREAT`,
`O_EXCL`, `O_TRUNC` and `O_APPEND`. `O_EXCL` requires `O_CREAT`; truncate and
append require writable access. `O_CREAT` alone preserves existing content.
Directories can only be opened with plain `O_RDONLY`. File `read` and `write`
check descriptor access even for zero bytes. Closing a file releases its handle;
call `sync` to report durable completion.

The `sync` syscall attempts every writable mounted volume, skips read-only
media and reports failure if any volume failed. File tools call it before
reporting success:

- `cp source new-destination` streams one file to an exclusive new destination.
  Existing destinations and self-copies through aliases are rejected. A failed
  copy leaves a reported partial destination available for inspection.
- `rm file...` removes files; `mkdir directory...` creates directories.
- `rmdir empty-directory...` removes empty directories.
- `mv source new-destination` renames within one volume, preserving existing
  destinations. It does not copy across volumes.
- The shell's `sync` command flushes writable volumes explicitly.

These commands are non-recursive. Quote paths with spaces using single or
double quotes, or escape the next character with a backslash. Quoted and
unquoted pieces concatenate, empty quotes produce an empty argument, and
malformed quotes or escapes reject the entire command. There is no expansion.

```text
mkdir "work files"
cp sys:README.TXT "work files/read me.txt"
mv 'work files/read me.txt' "work files/renamed file.txt"
rm "work files/renamed file.txt"
rmdir "work files"
sync
```

The end-to-end writable test creates a fresh 64 MiB scratch root from registered
prepared programs and `fsroot`, preserving the immutable shared userfs. QEMU
writes data and Unicode filenames through EL0, exercises quoted file commands,
and syncs. After the VM closes, the host checks `fsck.fat -n`, exact mtools
readback, removed paths, FAT mirrors and clean/error flags:

```bash
./build.sh --target qemu_virt
tests/run-qemu-write.py
```

Never attach the shared `build/userfs/aarch64/koraos.img` as writable media.
The ramdisk is a copy in RAM, so writing to it is always safe. Write tests on
disks use only scratch images; SD cards containing Pi firmware are outside this
workflow.

## Namespace operations and filenames

The kernel FAT32 API supports create, unlink, mkdir, rmdir and rename. The
namespace wrappers take cwd explicitly and resolve relative, volume-qualified
and assign-qualified paths. Rename stays within one mounted volume and reports
`FS_ERR_XDEV` across volumes. Existing destinations are preserved: rename does
not overwrite another entry. A case-only rename can update the source name.
Directories cannot be moved beneath themselves, and rmdir requires an empty
directory.

An open file or directory, current directory, assign target or retained ancestor
blocks removal and rename of that object. Holding a parent directory as cwd
does not block adding or removing an unpinned child. Removing or replacing an
assign releases its old directory references.

Created names use strictly validated UTF-8 and are encoded into VFAT UTF-16
entries, including surrogate pairs. The limit remains 255 UTF-16 code units
and 765 UTF-8 bytes. Overlong encodings, lone continuations, truncated sequences,
surrogate code points, out-of-range values and the VFAT padding unit U+FFFF are
rejected. FAT-forbidden characters, control bytes and trailing spaces or dots
are also rejected. Every name receives a collision-free ASCII 8.3 alias; both
the long name and alias can open the same file. Aliases avoid collisions with
existing short names and long names. LFN records carry ordinals, checksums,
a terminating zero when needed and padding, even across directory sector or
cluster boundaries.

Mkdir initializes `.` and `..`; moving a directory updates its parent link.
Directory growth allocates and links zeroed clusters, while deletion releases
the matching short entry and LFN slots for reuse. In-memory sector undo records
cover directory, FAT and allocation changes during a namespace operation.
A reported operation failure restores its previous namespace and allocations;
if rollback I/O fails, the volume retains recovery records for a later sync.
A successful operation followed by a failed sync retains its new state for retry.
This recovery handles reported errors and is not a power-loss journal.

The namespace sanitizer suite creates disposable 64 MiB images. Independent
checks inspect raw UTF-16 LFN records, aliases and directory parent links, run
`fsck.fat -n`, and read exact contents with mtools through short aliases. Heap
allocation failure and read, write, flush and persistent rollback errors are
injected before recovery and cold remount:

```bash
tests/run-fat-namespace-tests.sh
```

## Current layout of the image

```
/README.TXT
/docs/…                      example files, including Unicode-named ones
/bin/init  /bin/shell        the boot chain
/bin/ls  /bin/cat  /bin/echo  /bin/hello  /bin/gfxdemo
/bin/cp  /bin/rm  /bin/mkdir  /bin/rmdir  /bin/mv
```

## Limitations and deferred work

- **No sparse files.** Seeking beyond EOF and truncate growth are unsupported.
- **Console input is ASCII.** Kernel names and argv use UTF-8, and compiled
  programs can create Unicode names. Interactive console input currently
  filters non-ASCII bytes. Quotes make names containing ASCII spaces usable.
- **Pi drives are attached at boot.** The SD card and USB drives present at
  boot become volumes; there is no hot-plug yet. virt supports bare FAT32 and
  primary MBR FAT32 partitions on a VirtIO disk.
- **One sector per transfer.** The FAT32 layer reads and writes single
  sectors through its cache, which costs most on USB drives (each sector is a
  USB command; the USB driver reads ahead to soften it).
- **Primary MBR only.** GPT and extended/logical partitions are unsupported.
  The generated shared userfs remains a bare FAT32 volume.
- **No VFS.** FAT32 volumes share a namespace and a thin file/fd layer.
- **Case-insensitive matching is ASCII-only.** Non-ASCII case folding is not
  attempted.
- **Host astral-name creation.** Some mtools versions mis-encode astral input.
  The namespace suite creates these names through the kernel, verifies raw
  UTF-16 surrogate pairs independently, and reads contents through short aliases.

## VirtIO root disk on virt

```bash
./build.sh --target qemu_virt
KORA_QEMU_DISK=build/userfs/aarch64/koraos.img ./run-qemu.sh --target qemu_virt
```

Attach a bare FAT32 volume or a disk with primary MBR FAT32 partitions. The
first supported FAT32 partition is the boot root and must contain `/bin/init`
and `/bin/shell`. The launcher selects modern VirtIO MMIO;
legacy transport is unsupported. Media defaults to read-only; set
`KORA_QEMU_DISK_READONLY=off` only on a disposable image for write tests.
The block API supports `blk_read`, `blk_write` and negotiated `blk_flush`.
Writable file syscalls and tools operate on disposable writable media. The
driver's capacity is bounded by the current 32-bit sector API. Without a disk
the ramdisk is the root (if built with one); a configured broken disk does not
silently fall back. Timed-out devices retain DMA buffers
and reject subsequent requests until reboot.

## Block registry and primary MBR partitions

`blkdev_register(backend, type)` retains a physical backend and discovers its
mountable partitions. Backends must remain alive for the registry lifetime.
`blkdev_device_get` enumerates physical devices, including failed probes;
`blkdev_partition_get` enumerates supported views and their parent, start LBA,
and original MBR entry number. `blkdev_partition_io` returns the I/O view and
`blkdev_root` identifies the preferred boot view. Registration is synchronous;
filesystem operations need no additional locks under the kernel lock.

A bare FAT32 boot sector is recognized by its jump and BPB geometry. Otherwise
the sector must contain a signed MBR. FAT32 primary types `0x0b` and `0x0c`,
plus hidden variants `0x1b` and `0x1c`, produce views in table order. Occupied
entries must have valid boot indicators, nonzero starts and lengths, fit the
physical disk, and not overlap, including entries for other filesystems.
Malformed tables produce no partial views. Other primary filesystems are
ignored; protective GPT and extended entries reject the disk as unsupported.
A configured VirtIO disk with no usable boot view fails without ramdisk fallback.
The embedded ramdisk remains registered and mounted alongside external volumes.

To test MBR boot without altering the shared userfs artifact:

```bash
tests/create-mbr-image.py --output build/mbr-root.img build/userfs/aarch64/koraos.img
tests/run-qemu-smoke.py --target qemu_virt --disk build/mbr-root.img
```

The helper accepts up to four bare FAT32 source images, aligns partitions at
2048-sector boundaries, and atomically publishes a separate scratch image.
It preserves source images and never updates the shared producer artifact.
Every valid registered FAT32 view joins the namespace;
a failed preferred boot mount never substitutes another mounted volume.

## Volumes, paths and current directory

Device slots are named `df0:`, `df1:`, and so on across all backend types. A
slot represents a mountable partition, including a bare FAT32 image. An
embedded ramdisk normally occupies `df0:`; attached VirtIO partitions follow.
Slots retain their registry indices even if a partition fails to mount.
Volume names come from the FAT label, preferring the root-directory label over
the BPB copy. Mounting searches only the first 100 root-directory clusters for
that label, in one pass, so crafted media cannot stall boot; a label beyond them
falls back to the BPB copy. Matching of device names, volume names and path components is
case-insensitive for ASCII. Unicode filenames remain UTF-8 with exact
non-ASCII matching.

A unique volume label can select that volume, for example `extras:docs/file.txt`.
Repeated labels are ambiguous and must be addressed by device slot. Device
names take precedence over assigns, which take precedence over volume labels.
A device slot always remains available even when labels or assigns collide.

Each task starts at the boot volume root or inherits its parent's cwd. The
filesystem receives cwd explicitly; a child's directory change does not alter
its parent. Paths resolve as follows:

- `df1:docs/file.txt` and `extras:docs/file.txt` start at the selected root.
- `/docs/file.txt` starts at the root of the current volume.
- `file.txt` and `../file.txt` start at the current directory.
- `.` preserves the current directory; `..` at a volume root stays there.

Components are checked as the path is walked. `missing/..` fails because the
missing directory must exist before its parent can be selected. A filename
followed by `/..` also fails. Failed `chdir` calls preserve cwd. There is no
synthetic `/<volume>/...` tree.

`getcwd` writes a canonical volume-qualified string, for example `extras:docs`
or `extras:` at the root. It uses a unique label when that label resolves back
to the same volume; otherwise it uses the device slot. It returns `0` on
success and `-1` for an invalid or undersized buffer. Canonical paths within
a volume are bounded to 4095 bytes, excluding the NUL terminator. Qualified
input paths allow an additional 32 bytes for a prefix and separator, so the
longest valid `getcwd` result can be passed back to `chdir`. A filename
component retains the existing 765-byte UTF-8 limit.

The file syscalls and explicit program paths use this resolver. Bare program
names search only the `c:` assign. An unavailable `c:` or missing command fails;
there is no fallback to the boot volume or current volume's `/bin`.

## Assigns and shell navigation

Assigns are global, single-target names bound to existing directories. `sys:`
is fixed to the boot volume root; it cannot be replaced or removed. `c:` starts
at `sys:bin` and can be replaced or removed. Mount initialization requires the
boot command directory to exist. Assign targets are resolved when set, so
changing cwd or replacing/removing another assign cannot redirect an existing
assign. Relative targets use the caller's cwd.

There are at most 16 assigns, including defaults. Names occupy up to 31 bytes,
are matched case-insensitively for ASCII, and may carry an optional trailing
colon when passed to `assign`. Empty names, whitespace/control bytes, embedded
colons, slashes and reserved `df<digits>` names are rejected. Targets must exist
and be directories. A failed replacement preserves the previous target.

An assign path starts at its target directory. `c:hello` and `c:/hello` both
select `hello` within the command directory; the slash after the colon does
not discard the assign's directory anchor. `..` still follows the real parent
within that volume. Assigns can shadow volume labels; `getcwd` then uses a
device slot whenever the label would resolve to a different directory.

The shell provides these built-ins:

- `cd path` changes cwd; `cd` without arguments and `pwd` print it.
- `name:` alone changes cwd to that device, volume or assign target.
- `volumes` lists device slots and labels, marking boot and read-only volumes.
- `assign` lists assigns; `assign name target` creates or replaces one.
- `assign name` removes an assign. `sys:` remains fixed.

`ls` defaults to `.`. Other file commands accept the same path syntax, and
bare programs load through `c:` independently of the shell's cwd. Commands
allow up to 16 tokens, including the program name. Excess tokens and overlong
input lines are rejected as a whole; the shell drains an overlong line before
reading the next command.

```text
sys:
cd docs
pwd
ls
assign work sys:docs
work:
assign c extras:bin
extrahello
assign c sys:bin
```

The scratch EXTRAS fixture contains an `extrahello` command that is absent from
the boot image, providing a direct check that `c:` selects its configured
command directory. The `nsprobe` program checks cwd, descriptor binding, volume
and assign enumeration, immutable `sys:`, and assign creation/removal.

```bash
tests/create-multivolume-image.py --output build/multi-root.img build/userfs/aarch64/koraos.img
tests/run-qemu-smoke.py --target qemu_virt --disk build/multi-root.img --multi-volume
```

This fixture relabels separate copies as `BOOT` and `EXTRAS`, deliberately
leaves their BPB label copies stale, changes only the EXTRAS README and adds
its private `extrahello`, then checks that the original shared userfs SHA256
remains unchanged.

## Shared producer and dependencies

All current boards share AArch64 userland and the syscall ABI, so one userfs
artifact serves all four kernels and Debug/Release configurations. User headers,
program sources, runtime assembly and added/removed fsroot files trigger updates.
Image generation is atomic. Kernels depend on the prepared image and reassemble
their blob when it changes. `--userfs-only` builds it alone; `--userfs-dir` consumes
an explicitly prepared artifact without rebuilding. A future userland ISA/ABI or
configuration change needs a distinct artifact, rather than sharing incompatible
binaries.
