#!/usr/bin/env python3
"""QEMU SD card and USB drive writes: a --storage-writable kernel on disposable drives, checked on the host.

Builds scratch drive images (tests/create-sd-card-image.py), runs the smoke
test with them attached writable (the write steps copy files, make a
directory and sync), then checks each drive's FAT32 partition on the host:
fsck.fat, exact contents through mtools, and identical FAT copies. The kernel
must come from a build with --storage-writable, e.g.

  ./build.sh --target qemu_raspi3b --storage-writable --build-dir build/storage-writable \\
      --userfs-dir build/userfs/aarch64

--media picks the drives: an SD card (raspi3b, or raspi4b where it goes on the
Pi 4's EMMC2), a USB drive (raspi3b only: QEMU's raspi4b has no PCIe for the
xHCI), or both at once.
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


def verify(image, output, what):
    with tempfile.TemporaryDirectory(prefix="kora-storage-readback-") as temporary:
        volume = partition(image, Path(temporary) / "volume.img")
        fsck = subprocess.run(["fsck.fat", "-n", str(volume)], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / ("fsck-%s.log" % what)).write_text(fsck.stdout)
        if fsck.returncode:
            raise RuntimeError("post-QEMU fsck of the %s failed:\n%s" % (what, fsck.stdout))
        expected = {"/COPY.BIN": sd_card.big_content(), "/NEWDIR/NOTE.TXT": sd_card.MARKER.encode(),
                    "/BIG.BIN": sd_card.big_content(), "/README.TXT": sd_card.MARKER.encode()}
        for index, (name, content) in enumerate(expected.items()):
            target = Path(temporary) / str(index)
            subprocess.run(["mcopy", "-n", "-i", str(volume), "::" + name, str(target)], check=True)
            if target.read_bytes() != content:
                raise RuntimeError("wrong content on the %s: %s" % (what, name))
        raw = volume.read_bytes()
        reserved = struct.unpack_from("<H", raw, 14)[0]
        fat_count = raw[16]
        fat_sectors = struct.unpack_from("<I", raw, 36)[0]
        primary = raw[reserved * 512:(reserved + fat_sectors) * 512]
        for index in range(1, fat_count):
            start = (reserved + index * fat_sectors) * 512
            if raw[start:start + len(primary)] != primary:
                raise RuntimeError("FAT copies differ on the %s after sync" % what)
    print("QEMU %s writes: fsck clean, FAT copies identical, exact contents verified" % what)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--userfs-dir", default=str(ROOT / "build/userfs/aarch64"))
    parser.add_argument("--build-dir", default=str(ROOT / "build/storage-writable"),
                        help="build root of the --storage-writable kernel")
    parser.add_argument("--size-mib", type=int, choices=sd_card.CARD_MIB, default=4096,
                        help="SD card size (the USB drive is 64 MiB)")
    parser.add_argument("--target", choices=("qemu_raspi3b", "qemu_raspi4b"), default="qemu_raspi3b")
    parser.add_argument("--media", choices=("sd", "usb", "both"), default="sd")
    parser.add_argument("--out")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    if args.media != "sd" and args.target != "qemu_raspi3b":
        parser.error("USB drives need qemu_raspi3b (QEMU's raspi4b has no PCIe)")
    output = Path(args.out or Path(args.build_dir) / "debug" / args.target / "qemu-storage-write").resolve()
    output.mkdir(parents=True, exist_ok=True)
    shared = Path(args.userfs_dir).resolve() / "koraos.img"
    before = hashlib.sha256(shared.read_bytes()).hexdigest()
    drives = []  # (what, image)
    smoke = [str(ROOT / "tests/run-qemu-smoke.py"), "--target", args.target,
             "--build-dir", str(args.build_dir), "--qemu", args.qemu, "--out", str(output)]
    if args.media in ("sd", "both"):
        drives.append(("SD card", output / "card.img"))
        smoke += ["--sd", str(drives[-1][1]), "--sd-writable"]
    if args.media in ("usb", "both"):
        drives.append(("USB drive", output / "stick.img"))
        smoke += ["--usb-stick", str(drives[-1][1]), "--usb-stick-writable"]
    for what, image in drives:
        if image.exists():
            image.unlink()
        if what == "SD card":
            sd_card.create_image(shared, image, args.size_mib)
        else:
            sd_card.create_image(shared, image, 64, "USBSTICK")
    subprocess.run(smoke, check=True)
    for what, image in drives:
        verify(image, output, what)
    if hashlib.sha256(shared.read_bytes()).hexdigest() != before:
        raise RuntimeError("immutable shared userfs changed")
    print("immutable shared userfs SHA256 preserved: " + before)


if __name__ == "__main__":
    main()
