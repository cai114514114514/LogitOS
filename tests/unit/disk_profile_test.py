#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Opaque profile bytes across real mkfs.main, with independent image paths.

Small geometry keeps the gate cheap; format, Builder, guard, atomic replacement,
native journal recovery and fsck are production code. No real profile is read.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest.mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import mkfs
from disk_guard import image_guard
from disk_profile import CheckedImage, atomic_write

mkfs.TOTAL_BLOCKS = 2048
mkfs.INODE_COUNT = 128
mkfs.LOG_BLOCKS = 16
VALUES = {"/browser/cookies/jar.0": b"synthetic-cookie\0", "/browser/cookies/jar.1": b"synthetic-cookie\0",
          "/browser/storage.0": b"synthetic-local-storage", "/browser/storage.1": b"synthetic-local-storage",
          "/browser/future/nested": b"future-component-state", "/browser/empty-file": b"",
          "/browser/future/indirect": bytes(range(256)) * 240,
          "/browser/future/double-indirect": bytes(range(256)) * 16800}


def checksum(path):
    return hashlib.sha256(path.read_bytes()).digest()


def seed(path):
    builder = mkfs.Builder()
    builder.add_file("/browser.aex", b"old-synthetic-program")
    for dest, content in VALUES.items():
        builder.add_file(dest, content)
    builder.get_or_make_dir(["browser", "empty-directory"])
    for ino in range(builder.next_ino):
        mode = 0o700 if builder.itype[ino] == mkfs.T_DIR else 0o600
        builder.metadata[ino] = struct.pack("<qqqIII", 10, 20, 30, mkfs.MODE_SET | mode, 123, 456)
    data, _ = builder.serialize()
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", required=True)
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()
    helper = str(Path(args.helper).resolve())
    passed = 0
    def check(ok, name):
        nonlocal passed
        if not ok:
            raise AssertionError(name)
        passed += 1
        print("ok: " + name)
    with tempfile.TemporaryDirectory(prefix="logit-profile-test-") as work:
        work = Path(work)
        disk = work / "disk.img"
        packed = work / "program.aex"
        packed.write_bytes(b"new-synthetic-program")
        shim = work / "pack.py"
        shim.write_text("import sys\nsys.path.insert(0," + repr(str(ROOT / "tools")) + ")\n"
                        "import mkfs\nmkfs.TOTAL_BLOCKS=2048\nmkfs.INODE_COUNT=128\nmkfs.LOG_BLOCKS=16\nmkfs.main()\n")
        def rebuild(preserve=True, image=disk, spec=None):
            cmd = [sys.executable, str(shim)]
            if preserve:
                cmd += ["--preserve", "/browser", "--snapshot-helper", helper]
            cmd += [str(image), spec or str(packed) + ":/browser.aex"]
            return subprocess.run(cmd, capture_output=True, text=True)
        def payload(image, path):
            source = CheckedImage(image)
            ino = source.resolve(path)
            return None if ino is None else source.payload(ino)
        seed(disk)
        if args.negative:
            result = rebuild(False)
            if result.returncode:
                raise AssertionError("negative control failed before rebuilding")
            if payload(disk, "/browser/storage.0") != VALUES["/browser/storage.0"]:
                print("FAIL: rebuilding without preservation loses the existing browser profile")
                return 1
            raise AssertionError("negative control unexpectedly retained state")
        result = rebuild()
        check(result.returncode == 0, "ordinary rebuild succeeds with checked profile preservation")
        check(payload(disk, "/browser.aex") == packed.read_bytes(), "new packaged program replaces the old program")
        check(all(payload(disk, key) == val for key, val in VALUES.items()), "entire browser tree retains exact opaque bytes including indirect and double-indirect files")
        image = CheckedImage(disk)
        directory = image.resolve("/browser/empty-directory")
        check(directory is not None and image.directory(directory) == {}, "empty profile directories survive")
        ino = image.resolve("/browser/cookies/jar.0")
        metadata = image.inode(ino)[2][mkfs.OFF_ATIME:mkfs.OFF_GID + 4]
        check(metadata == struct.pack("<qqqIII", 10, 20, 30, mkfs.MODE_SET | 0o600, 123, 456), "private mode owner and inode times survive")
        check(disk.stat().st_mode & 0o777 == 0o600, "host disk containing profile is private")
        before = checksum(disk)
        with disk.open("rb"):
            result = rebuild()
            check(result.returncode != 0 and "disk is open by PID" in result.stderr and checksum(disk) == before,
                  "legacy or directly launched open disk is refused without changes")
        with image_guard(disk):
            result = rebuild()
            check(result.returncode != 0 and "already held" in result.stderr and checksum(disk) == before,
                  "shared lifecycle lock refuses a concurrent builder or launcher")
        result = rebuild(spec=str(work / "missing-file") + ":/missing")
        check(result.returncode != 0 and checksum(disk) == before, "pack input failure leaves previous disk byte-identical")
        with unittest.mock.patch("os.fsync", side_effect=OSError("synthetic fsync failure")):
            try:
                atomic_write(disk, b"incomplete-replacement")
            except OSError:
                pass
            else:
                raise AssertionError("injected write failure was ignored")
        check(checksum(disk) == before, "temporary write failure cannot truncate the old image")
        corrupt = work / "corrupt.img"
        seed(corrupt)
        raw = bytearray(corrupt.read_bytes())
        source = CheckedImage(corrupt)
        ino = source.resolve("/browser/storage.0")
        struct.pack_into("<I", raw, source.sb[7] * mkfs.BS + ino * mkfs.INODE_SIZE + mkfs.OFF_DIRECT, 0xffffffff)
        corrupt.write_bytes(raw)
        bad_before = checksum(corrupt)
        result = rebuild(image=corrupt)
        check(result.returncode != 0 and checksum(corrupt) == bad_before, "filesystem corruption refuses migration without modifying source")
        journal = work / "journal.img"
        seed(journal)
        source = CheckedImage(journal)
        ino = source.resolve("/browser/storage.0")
        target = struct.unpack_from("<I", source.inode(ino)[2], mkfs.OFF_DIRECT)[0]
        replacement = b"committed-new-storage!"
        replacement = replacement[:len(VALUES["/browser/storage.0"])].ljust(len(VALUES["/browser/storage.0"]), b"!")
        raw = bytearray(journal.read_bytes())
        body = replacement.ljust(mkfs.BS, b"\0")
        header = bytearray(mkfs.BS)
        struct.pack_into("<5I", header, 0, mkfs.LOG_MAGIC, 1, 1, zlib.crc32(body), target)
        struct.pack_into("<I", header, mkfs.BS - 4, zlib.crc32(header[:-4]))
        log = source.sb[11] * mkfs.BS
        raw[log:log + mkfs.BS] = header
        raw[log + mkfs.BS:log + 2 * mkfs.BS] = body
        journal.write_bytes(raw)
        check(payload(journal, "/browser/storage.0") == VALUES["/browser/storage.0"], "journal control leaves home data at the old committed snapshot")
        result = rebuild(image=journal)
        check(result.returncode == 0 and payload(journal, "/browser/storage.0") == replacement,
              "native mount journal recovery retains the newest committed profile")
        link = work / "link.img"
        link.symlink_to(disk)
        result = rebuild(image=link)
        check(result.returncode != 0 and checksum(disk) == before, "symlink destination cannot redirect replacement")
        result = rebuild(image=work)
        check(result.returncode != 0 and checksum(disk) == before, "non-regular destination is refused")
    print("disk-profile: %d checks passed" % passed)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        sys.exit("FAIL: " + str(exc))
