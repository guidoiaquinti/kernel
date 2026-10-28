#!/usr/bin/env python3
"""Check create-ramdisk-image.py: size parsing, the blob format and faithful contents."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("ramdisk_image", ROOT / "create-ramdisk-image.py")
ramdisk_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ramdisk_image)
ENV = ramdisk_image.mtools_env()


def listing(image):
    """mdir's recursive listing: names in on-disk directory order."""
    return subprocess.run(["mdir", "-/", "-b", "-i", str(image), "::"], check=True, env=ENV,
                          stdout=subprocess.PIPE, text=True).stdout.splitlines()


def read(image, path):
    return subprocess.run(["mtype", "-i", str(image), "::" + path], check=True, env=ENV,
                          stdout=subprocess.PIPE).stdout


class SizeTests(unittest.TestCase):
    def test_units(self):
        parse = ramdisk_image.parse_size
        self.assertEqual(parse("16M"), 16 << 20)
        self.assertEqual(parse("1G"), 1 << 30)
        self.assertEqual(parse("512k"), 512 << 10)
        self.assertEqual(parse("64MiB"), 64 << 20)
        self.assertEqual(parse("2gb"), 2 << 30)
        self.assertEqual(parse("8192"), 8192)

    def test_rejected(self):
        for text in ("", "M", "16X", "1.5G", "-1M", "0", "0M", "1000", "16 M B", "1T"):
            with self.assertRaises(ValueError, msg=text):
                ramdisk_image.parse_size(text)


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kora-ramdisk-tests-")
        self.root = Path(self.temporary.name)
        # A small userfs: a directory listed before a file, a nested file, a
        # UTF-8 long name, and a file spanning several 4 KiB runs.
        self.userfs = self.root / "userfs.img"
        with self.userfs.open("wb") as stream:
            stream.truncate(2 << 20)
        subprocess.run(["mformat", "-F", "-v", "KORAOS", "-i", str(self.userfs), "::"], check=True, env=ENV)
        files = self.root / "files"
        files.mkdir()
        (files / "README.TXT").write_text("ramdisk test\n")
        (files / "résumé 東京.txt").write_text("unicode\n")
        self.big = bytes((i * 31 + 7) & 0xff for i in range(20000))
        (files / "big.bin").write_bytes(self.big)
        for command in (["mmd", "-i", str(self.userfs), "::/docs"],
                        ["mcopy", "-i", str(self.userfs), str(files / "README.TXT"), "::/README.TXT"],
                        ["mmd", "-i", str(self.userfs), "::/bin"],
                        ["mcopy", "-i", str(self.userfs), str(files / "résumé 東京.txt"), "::/docs/résumé 東京.txt"],
                        ["mcopy", "-i", str(self.userfs), str(files / "big.bin"), "::/bin/init"]):
            subprocess.run(command, check=True, env=ENV)

    def tearDown(self):
        self.temporary.cleanup()

    def build(self, size):
        blob = self.root / "ramdisk.blob"
        ramdisk_image.create_image(self.userfs, blob, size)
        volume = self.root / "volume.img"
        volume.write_bytes(ramdisk_image.unpack(blob.read_bytes()))
        return blob, volume

    def test_round_trip(self):
        before = self.userfs.read_bytes()
        blob, volume = self.build(64 << 20)
        self.assertEqual(volume.stat().st_size, 64 << 20)
        self.assertLess(blob.stat().st_size, 1 << 20, "only the used parts are embedded")
        self.assertEqual(listing(volume), listing(self.userfs), "same names, same directory order")
        self.assertEqual(read(volume, "/bin/init"), self.big)
        self.assertEqual(read(volume, "/docs/résumé 東京.txt"), b"unicode\n")
        fsck = subprocess.run(["fsck.fat", "-n", str(volume)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(fsck.returncode, 0, fsck.stdout)
        self.assertEqual(self.userfs.read_bytes(), before, "the shared userfs is only read")

    def test_blob_format(self):
        blob, _ = self.build(16 << 20)
        data = blob.read_bytes()
        magic, size, count, reserved = ramdisk_image.HEADER.unpack_from(data, 0)
        self.assertEqual((magic, size, reserved), (b"KORARD1\0", 16 << 20, 0))
        position, end = ramdisk_image.HEADER.size, 0
        for _ in range(count):
            offset, length = ramdisk_image.RUN.unpack_from(data, position)
            self.assertTrue(offset % 4096 == 0 and length % 4096 == 0 and length > 0 and offset >= end)
            self.assertEqual(position % 8, 0, "runs stay 8-byte aligned for the kernel's word copy")
            position += ramdisk_image.RUN.size + length
            end = offset + length
        self.assertEqual(position, len(data))
        self.assertLessEqual(end, size)

    def test_too_small_and_too_large(self):
        with self.assertRaises(ValueError):
            ramdisk_image.create_image(self.userfs, self.root / "small.blob", 8 << 10)
        with self.assertRaises(ValueError):
            ramdisk_image.create_image(self.userfs, self.root / "huge.blob", 4 << 30)
        with self.assertRaises(ValueError):
            ramdisk_image.create_image(self.userfs, self.userfs, 16 << 20)

    def test_board_limit(self):
        script = [sys.executable, str(ROOT / "create-ramdisk-image.py"), str(self.userfs),
                  "--output", str(self.root / "pi3.blob"), "--below", "1G", "--board", "a Pi 3"]
        refused = subprocess.run(script + ["--size", "1G"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("does not fit a Pi 3", refused.stdout)
        accepted = subprocess.run(script + ["--size", "16M"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True)
        self.assertEqual(accepted.returncode, 0, accepted.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
