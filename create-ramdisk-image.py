#!/usr/bin/env python3
"""Build the ramdisk a kernel embeds: the shared userfs contents in a FAT32 volume of a chosen size.

The volume is formatted at its full size (e.g. 16M or 1G), so the kernel can
write to all of it, but only its non-zero parts are embedded: the blob holds
the volume's size and its non-zero 4 KiB runs (boot sector, the start of each
FAT, the files). At boot the kernel zeroes a RAM region of that size and
copies the runs into it (src/fs/ramdisk_image.c reads the same format):

  header   "KORARD1\\0", u64 volume bytes, u32 run count, u32 reserved
  runs     u64 offset, u64 length, then `length` bytes; offsets and lengths
           are multiples of 4096, all little-endian

The shared userfs image is only read.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

MAGIC = b"KORARD1\0"
HEADER = struct.Struct("<8sQII")
RUN = struct.Struct("<QQ")
CHUNK = 4096
# The kernel maps the low 4 GB and its block layer counts 32-bit sectors;
# the largest RAM region any supported board offers is the Pi 4's 3 GB above
# its first gigabyte.
MAX_BYTES = 3 * 1024 ** 3
UNITS = {"": 1, "K": 1024, "M": 1024 ** 2, "G": 1024 ** 3}


def parse_size(text):
    """'16M' -> bytes: a whole number with an optional K, M or G (powers of
    1024, optionally written KB/KiB). The result must be a positive multiple
    of 4 KiB."""
    m = re.fullmatch(r"\s*(\d+)\s*([KMG]?)(?:I?B)?\s*", text.upper())
    if m is None:
        raise ValueError("size %r: use a number with K, M or G, e.g. 16M or 1G" % text)
    size = int(m.group(1)) * UNITS[m.group(2)]
    if size == 0 or size % CHUNK:
        raise ValueError("size %r must be a positive multiple of 4K" % text)
    return size


def runs(image):
    """The image's non-zero CHUNK-aligned runs, as (offset, length)."""
    found = []
    with image.open("rb") as stream:
        offset = 0
        while True:
            chunk = stream.read(CHUNK)
            if not chunk:
                break
            if chunk.count(0) != len(chunk):
                if found and found[-1][0] + found[-1][1] == offset:
                    found[-1] = (found[-1][0], found[-1][1] + CHUNK)
                else:
                    found.append((offset, CHUNK))
            offset += len(chunk)
    return found


def pack(image, size):
    """The blob for `image`, a volume of `size` bytes."""
    found = runs(image)
    parts = [HEADER.pack(MAGIC, size, len(found), 0)]
    with image.open("rb") as stream:
        for offset, length in found:
            stream.seek(offset)
            parts.append(RUN.pack(offset, length))
            parts.append(stream.read(length))
    return b"".join(parts)


def unpack(blob):
    """The volume a blob describes (for tests; the kernel does the same)."""
    magic, size, count, _ = HEADER.unpack_from(blob, 0)
    if magic != MAGIC:
        raise ValueError("not a ramdisk blob")
    volume = bytearray(size)
    position = HEADER.size
    for _ in range(count):
        offset, length = RUN.unpack_from(blob, position)
        position += RUN.size
        volume[offset:offset + length] = blob[position:position + length]
        position += length
    return bytes(volume)


def mtools_env():
    """mtools converts long names through the locale: keep it UTF-8."""
    env = dict(os.environ)
    env["LC_ALL"] = "en_US.UTF-8" if os.uname().sysname == "Darwin" else "C.UTF-8"
    return env


def create_image(userfs, output, size, label="KORAOS"):
    userfs, output = Path(userfs), Path(output)
    if size > MAX_BYTES:
        raise ValueError("a ramdisk can be at most 3G")
    if output.resolve() == userfs.resolve():
        raise ValueError("the ramdisk must be separate from the shared userfs")
    before = hashlib.sha256(userfs.read_bytes()).digest()
    env = mtools_env()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kora-ramdisk-", dir=output.parent) as temporary:
        volume = Path(temporary) / "volume.img"
        with volume.open("wb") as stream:
            stream.truncate(size)
        formatted = subprocess.run(["mformat", "-F", "-v", label, "-i", str(volume), "::"], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if formatted.returncode:
            raise ValueError("cannot format a %d KiB FAT32 volume (too small?):\n%s"
                             % (size // 1024, formatted.stdout))
        tree = Path(temporary) / "tree"
        tree.mkdir()
        subprocess.run(["mcopy", "-s", "-n", "-Q", "-i", str(userfs), "::/*", str(tree)], check=True, env=env)
        # Recreate entries one by one in the image's own directory order
        # (mdir lists it), which a host directory would not keep.
        listing = subprocess.run(["mdir", "-/", "-b", "-i", str(userfs), "::"], check=True, env=env,
                                 stdout=subprocess.PIPE, text=True).stdout.splitlines()
        for entry in listing:
            path = entry[len("::/"):]
            if not path:
                continue
            command = (["mmd", "-i", str(volume), "::/" + path.rstrip("/")] if path.endswith("/") else
                       ["mcopy", "-Q", "-i", str(volume), str(tree / path), "::/" + path])
            copy = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if copy.returncode:
                raise ValueError("cannot put %s in a %d KiB ramdisk (does it fit?):\n%s"
                                 % (path, size // 1024, copy.stdout))
        blob = pack(volume, size)
        staged = Path(temporary) / "blob"
        staged.write_bytes(blob)
        os.replace(staged, output)
    if hashlib.sha256(userfs.read_bytes()).digest() != before:
        raise RuntimeError("shared userfs changed while building the ramdisk")
    return len(blob)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("userfs", help="the shared userfs image (koraos.img)")
    parser.add_argument("--size", required=True, help="volume size, e.g. 16M or 1G")
    parser.add_argument("--below", help="sizes from this one up do not fit the board, e.g. 1G")
    parser.add_argument("--board", default="this board", help="the board, for the --below message")
    parser.add_argument("--output", required=True, help="the blob to embed")
    args = parser.parse_args()
    for tool in ("mformat", "mcopy"):
        if shutil.which(tool) is None:
            parser.error("%s not found: install mtools" % tool)
    try:
        size = parse_size(args.size)
        if args.below and size >= parse_size(args.below):
            raise ValueError("a %s ramdisk does not fit %s; use less than %s" % (args.size, args.board, args.below))
        embedded = create_image(args.userfs, args.output, size)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    shown = "%d MiB" % (size >> 20) if size % (1 << 20) == 0 else "%d KiB" % (size >> 10)
    print("ramdisk: %s FAT32 volume, %d KiB embedded" % (shown, embedded // 1024))


if __name__ == "__main__":
    main()
