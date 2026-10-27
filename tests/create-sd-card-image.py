#!/usr/bin/env python3
"""Build a disposable SD card (or USB drive) image for QEMU from a copy of the shared userfs.

The card holds one MBR partition: the userfs volume relabelled SDCARD (or
another --label, e.g. USBSTICK for a QEMU usb-storage drive), plus
README.TXT (a marker) and BIG.BIN (deterministic bytes, large enough for
multi-block reads). QEMU wants SD card images sized to a power of two, so the
image is padded (sparsely) to 64 MiB, which QEMU presents as a standard
capacity card, or 4 GiB, a high-capacity (SDHC, block-addressed) one (QEMU
makes cards above 2 GiB high capacity). The
shared userfs is never modified.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location("mbr_image", Path(__file__).with_name("create-mbr-image.py"))
mbr_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mbr_image)

CARD_MIB = (64, 4096)  # standard (byte-addressed) and high capacity in QEMU
MARKER = "Storage marker: read through the SD or USB driver.\n"
BIG_BYTES = 300 * 1024 + 123  # not a whole number of sectors or clusters


def big_content():
    """BIG.BIN: a byte pattern no cluster or sector repeats exactly."""
    return bytes((i * 7 + (i >> 9) * 13 + (i >> 17)) & 0xff for i in range(BIG_BYTES))


def fnv1a64(data):
    """The checksum tests/user/sumprobe.c prints."""
    h = 0xcbf29ce484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return h


def create_image(source, output, mib=64, label="SDCARD"):
    source, output = Path(source), Path(output)
    if source.resolve() == output.resolve():
        raise ValueError("card image must be separate from shared userfs")
    before = hashlib.sha256(source.read_bytes()).digest()
    with tempfile.TemporaryDirectory(prefix="kora-sd-fixture-") as temporary:
        volume = Path(temporary) / "sdcard.img"
        shutil.copyfile(source, volume)
        subprocess.run(["mlabel", "-i", str(volume), "::" + label], check=True)
        marker = Path(temporary) / "readme.txt"
        marker.write_text(MARKER)
        big = Path(temporary) / "big.bin"
        big.write_bytes(big_content())
        subprocess.run(["mcopy", "-o", "-i", str(volume), str(marker), "::/README.TXT"], check=True)
        subprocess.run(["mcopy", "-o", "-i", str(volume), str(big), "::/BIG.BIN"], check=True)
        mbr_image.create_image([volume], output)
    if mib not in CARD_MIB:
        raise ValueError("card size must be one of %s MiB" % (CARD_MIB,))
    if output.stat().st_size > mib * 1024 * 1024:
        raise ValueError("card contents exceed %d MiB" % mib)
    with output.open("r+b") as stream:
        stream.truncate(mib * 1024 * 1024)
    if hashlib.sha256(source.read_bytes()).digest() != before:
        raise RuntimeError("shared userfs changed during SD fixture generation")
    print("created image %s (%d MiB, %s partition); shared userfs preserved" % (output, mib, label))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("source")
    parser.add_argument("--output", required=True)
    parser.add_argument("--size-mib", type=int, choices=CARD_MIB, default=64)
    parser.add_argument("--label", default="SDCARD", help="volume label (default SDCARD)")
    args = parser.parse_args()
    try:
        create_image(args.source, args.output, args.size_mib, args.label)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
