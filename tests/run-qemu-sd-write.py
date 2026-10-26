#!/usr/bin/env python3
"""QEMU raspi3b/raspi4b SD card writes: a --sd-writable kernel on a disposable card, checked on the host.

Builds a scratch card image (tests/create-sd-card-image.py), runs the smoke
test with it attached writable (the SD write steps copy files, make a
directory and sync), then checks the card's FAT32 partition on the host:
fsck.fat, exact contents through mtools, and identical FAT copies. The kernel
must come from a build with --sd-writable, e.g.

  ./build.sh --target qemu_raspi3b --sd-writable --build-dir build/sd-writable \\
      --userfs-dir build/userfs/aarch64

(or --target qemu_raspi4b, then run this with --target qemu_raspi4b: the card
goes on the Pi 4's EMMC2).
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("sd_card", ROOT / "tests/create-sd-card-image.py")
sd_card = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd_card)


def partition(image, scratch):
    """Copy the card's first MBR partition out to `scratch`."""
    with image.open("rb") as stream:
        mbr = stream.read(512)
        start, sectors = struct.unpack_from("<II", mbr, 446 + 8)
        if mbr[510:512] != b"\x55\xaa" or not sectors:
            raise RuntimeError("card image lost its MBR")
        stream.seek(start * 512)
        scratch.write_bytes(stream.read(sectors * 512))
    return scratch


def verify(image, output):
    with tempfile.TemporaryDirectory(prefix="kora-sd-readback-") as temporary:
        volume = partition(image, Path(temporary) / "volume.img")
        fsck = subprocess.run(["fsck.fat", "-n", str(volume)], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / "fsck.log").write_text(fsck.stdout)
        if fsck.returncode:
            raise RuntimeError("post-QEMU fsck failed:\n" + fsck.stdout)
        expected = {"/COPY.BIN": sd_card.big_content(), "/NEWDIR/NOTE.TXT": sd_card.MARKER.encode(),
                    "/BIG.BIN": sd_card.big_content(), "/README.TXT": sd_card.MARKER.encode()}
        for index, (name, content) in enumerate(expected.items()):
            target = Path(temporary) / str(index)
            subprocess.run(["mcopy", "-n", "-i", str(volume), "::" + name, str(target)], check=True)
            if target.read_bytes() != content:
                raise RuntimeError("wrong content on the card: " + name)
        raw = volume.read_bytes()
        reserved = struct.unpack_from("<H", raw, 14)[0]
        fat_count = raw[16]
        fat_sectors = struct.unpack_from("<I", raw, 36)[0]
        primary = raw[reserved * 512:(reserved + fat_sectors) * 512]
        for index in range(1, fat_count):
            start = (reserved + index * fat_sectors) * 512
            if raw[start:start + len(primary)] != primary:
                raise RuntimeError("FAT copies differ after sync")
    print("QEMU SD writes: fsck clean, FAT copies identical, exact contents verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--userfs-dir", default=str(ROOT / "build/userfs/aarch64"))
    parser.add_argument("--build-dir", default=str(ROOT / "build/sd-writable"),
                        help="build root of the --sd-writable kernel")
    parser.add_argument("--size-mib", type=int, choices=sd_card.CARD_MIB, default=4096)
    parser.add_argument("--target", choices=("qemu_raspi3b", "qemu_raspi4b"), default="qemu_raspi3b")
    parser.add_argument("--out")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    output = Path(args.out or Path(args.build_dir) / "debug" / args.target / "qemu-sd-write").resolve()
    output.mkdir(parents=True, exist_ok=True)
    shared = Path(args.userfs_dir).resolve() / "koraos.img"
    before = hashlib.sha256(shared.read_bytes()).hexdigest()
    image = output / "card.img"
    if image.exists():
        image.unlink()
    sd_card.create_image(shared, image, args.size_mib)
    subprocess.run([str(ROOT / "tests/run-qemu-smoke.py"), "--target", args.target,
                    "--build-dir", str(args.build_dir), "--sd", str(image), "--sd-writable",
                    "--qemu", args.qemu, "--out", str(output)], check=True)
    verify(image, output)
    if hashlib.sha256(shared.read_bytes()).hexdigest() != before:
        raise RuntimeError("immutable shared userfs changed")
    print("immutable shared userfs SHA256 preserved: " + before)


if __name__ == "__main__":
    main()
