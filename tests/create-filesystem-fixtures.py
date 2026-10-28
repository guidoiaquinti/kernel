#!/usr/bin/env python3
"""Produce real mtools FAT32 volumes and narrowly corrupted read-only fixtures."""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import tempfile

SECTOR = 512


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def geometry(image):
    reserved = struct.unpack_from("<H", image, 14)[0]
    fats = image[16]
    fatsz = struct.unpack_from("<I", image, 36)[0]
    return reserved, fatsz, reserved + fats * fatsz, image[13]


def root_entries(image):
    _, _, data, spc = geometry(image)
    root = struct.unpack_from("<I", image, 44)[0]
    start = (data + (root - 2) * spc) * SECTOR
    return range(start, start + spc * SECTOR, 32)


DELETED = b"\xe5" + bytes(31)


def set_fat(image, cluster, value):
    reserved, fatsz, _, _ = geometry(image)
    for fat in range(image[16]):
        struct.pack_into("<I", image, (reserved + fat * fatsz) * SECTOR + cluster * 4, value)


def open_root(image):
    """Replace the root cluster's end-of-directory marker with deleted entries."""
    image = bytearray(image)
    for offset in root_entries(image):
        if image[offset] == 0:
            image[offset:offset + 32] = DELETED
    return image


def long_root(image, label_cluster, label, chain=400):
    """Continue the root directory over a chain whose links hop between 16 FAT
    sectors, so a walk restarted from the root cluster misses the sector cache."""
    image = open_root(image)
    reserved, _, data, spc = geometry(image)
    root = struct.unpack_from("<I", image, 44)[0]
    clusters = [128 * (1 + index % 16) + 64 + index // 16 for index in range(chain)]
    for cluster in clusters:
        if struct.unpack_from("<I", image, reserved * SECTOR + cluster * 4)[0]:
            raise RuntimeError("long-root cluster already allocated")
    for current, following in zip([root] + clusters, clusters + [0x0fffffff]):
        set_fat(image, current, following)
    for index, cluster in enumerate(clusters):
        entries = [DELETED] * (spc * SECTOR // 32)
        if index == label_cluster:
            entries[0] = label.ljust(11).encode() + b"\x08" + bytes(20)
        start = (data + (cluster - 2) * spc) * SECTOR
        image[start:start + spc * SECTOR] = b"".join(entries)
    return image


def make_fixture(output, label, size, text, temporary):
    with output.open("wb") as stream:
        stream.truncate(size * 1024 * 1024)
    run("mformat", "-i", str(output), "-F", "-c", "1", "-v", label, "::")
    run("mmd", "-i", str(output), "::/docs", "::/docs/child", "::/bin")
    payload = temporary / "payload.txt"
    payload.write_text(text)
    for destination in ("::/marker.txt", "::/docs/child/nested.txt",
                        "::/a-long-invalid-name.txt", "::/docs/résumé-notes.txt"):
        run("mcopy", "-i", str(output), str(payload), destination)
    command = "bootcmd" if label == "ALPHA" else "extrahello"
    run("mcopy", "-i", str(output), str(payload), "::/bin/" + command)
    large = temporary / "large.bin"
    large.write_bytes(bytes((index * 37 + 11) % 256 for index in range(5000)))
    run("mcopy", "-i", str(output), str(large), "::/large.bin")
    image = bytearray(output.read_bytes())
    image[71:82] = b"STALE      "
    output.write_bytes(image)
    return image


def create_fixtures(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kora-fs-fixture-") as temporary:
        temporary = Path(temporary)
        alpha = make_fixture(directory / "alpha.img", "ALPHA", 2, "alpha volume\n", temporary)
        make_fixture(directory / "beta.img", "BETA", 4, "beta volume\n", temporary)
        duplicate = bytearray(alpha)
        (directory / "duplicate.img").write_bytes(duplicate)
        collision = bytearray(alpha)
        for offset in root_entries(collision):
            if collision[offset + 11] == 0x08:
                collision[offset:offset + 11] = b"DF0        "
                break
        (directory / "collision.img").write_bytes(collision)
        invalid_geometry = bytearray(alpha)
        struct.pack_into("<I", invalid_geometry, 36, 0xffffffff)
        (directory / "bad-geometry.img").write_bytes(invalid_geometry)
        bad_cluster = bytearray(alpha)
        loop = bytearray(alpha)
        for offset in root_entries(alpha):
            if alpha[offset:offset + 11] == b"LARGE   BIN":
                first = (struct.unpack_from("<H", alpha, offset + 20)[0] << 16
                         | struct.unpack_from("<H", alpha, offset + 26)[0])
                reserved, fatsz, _, _ = geometry(alpha)
                for fat in range(alpha[16]):
                    position = (reserved + fat * fatsz) * SECTOR + first * 4
                    struct.pack_into("<I", bad_cluster, position, 0x0ffffff7)
                    struct.pack_into("<I", loop, position, first)
                break
        else:
            raise RuntimeError("mtools large-file short entry missing")
        (directory / "bad-chain.img").write_bytes(bad_cluster)
        (directory / "loop-chain.img").write_bytes(loop)
        wrong_checksum, incomplete = bytearray(alpha), bytearray(alpha)
        lfn_found = False
        for offset in root_entries(alpha):
            if alpha[offset + 11] == 0x0f:
                # The first root LFN belongs to a-long-invalid-name.txt.
                wrong_checksum[offset + 13] ^= 0x80
                incomplete[offset] = 0xe5
                lfn_found = True
                break
        if not lfn_found:
            raise RuntimeError("mtools long-name entry missing")
        (directory / "bad-lfn-checksum.img").write_bytes(wrong_checksum)
        (directory / "incomplete-lfn.img").write_bytes(incomplete)
        zero_ordinal = bytearray(alpha)
        positions = list(root_entries(alpha))
        for index, offset in enumerate(positions):
            if alpha[offset + 11] == 0x0f and alpha[offset] == 1:
                short = positions[index + 1]
                end = positions[-1] + 32
                zero_ordinal[short + 32:end] = alpha[short:end - 32]
                zero_ordinal[short:short + 32] = alpha[offset:offset + 32]
                zero_ordinal[short] = 0x20
                break
        else:
            raise RuntimeError("mtools final LFN ordinal missing")
        (directory / "zero-ordinal-lfn.img").write_bytes(zero_ordinal)
        fallback = bytearray(alpha)
        for offset in root_entries(fallback):
            if fallback[offset + 11] == 0x08:
                fallback[offset] = 0xe5
                break
        fallback[71:82] = b"FALLBACK   "
        (directory / "bpb-label.img").write_bytes(fallback)
        (directory / "deep-label.img").write_bytes(long_root(fallback, 399, "DEEP"))
        (directory / "late-label.img").write_bytes(long_root(fallback, 2, "LATE"))
        root = struct.unpack_from("<I", fallback, 44)[0]
        root_loop, root_bad_chain = open_root(fallback), open_root(fallback)
        set_fat(root_loop, root, root)
        set_fat(root_bad_chain, root, 0x0ffffff7)
        (directory / "root-loop.img").write_bytes(root_loop)
        (directory / "root-bad-chain.img").write_bytes(root_bad_chain)
        boundary = directory / "boundary.img"
        shutil.copyfile(directory / "alpha.img", boundary)
        run("mlabel", "-i", str(boundary), "::LONGVOLUME1")
        components = ["a" * 254] * 16 + ["b" * 14]
        path = ""
        for component in components:
            path += "/" + component
            run("mmd", "-i", str(boundary), "::" + path)
        if len(path.encode()) != 4095:
            raise RuntimeError("boundary fixture canonical path is not maximal")
        (directory / "boundary-path.txt").write_text(path)
    print("created filesystem fixtures in " + str(directory))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory")
    create_fixtures(parser.parse_args().directory)


if __name__ == "__main__":
    main()
