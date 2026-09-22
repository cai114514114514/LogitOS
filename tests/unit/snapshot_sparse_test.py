#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A checked profile snapshot keeps raw bytes while leaving zero blocks sparse."""

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import mkfs
from disk_profile import CheckedImage


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def snapshot(helper, source, output):
    result = subprocess.run([str(helper), str(source), str(output)],
                            capture_output=True, text=True)
    check(result.returncode == 0, "snapshot helper failed: " + result.stderr)
    check("profile snapshot: checked" in result.stdout,
          "snapshot helper did not report fsck completion")
    check(output.stat().st_size == source.stat().st_size,
          "snapshot changed the raw disk's logical size")
    check(output.stat().st_mode & 0o777 == 0o600,
          "snapshot lost its private host permissions")
    return output.stat().st_blocks * 512


def committed_journal(image):
    source = CheckedImage(image)
    inode = source.resolve("/browser/state")
    block = struct.unpack_from("<I", source.inode(inode)[2], mkfs.OFF_DIRECT)[0]
    replacement = b"new-state"
    raw = bytearray(image.read_bytes())
    body = replacement.ljust(mkfs.BS, b"\0")
    header = bytearray(mkfs.BS)
    struct.pack_into("<5I", header, 0, mkfs.LOG_MAGIC, 1, 1,
                     zlib.crc32(body), block)
    struct.pack_into("<I", header, mkfs.BS - 4, zlib.crc32(header[:-4]))
    log = source.sb[11] * mkfs.BS
    raw[log:log + mkfs.BS] = header
    raw[log + mkfs.BS:log + 2 * mkfs.BS] = body
    image.write_bytes(raw)
    return replacement


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", required=True, type=Path)
    parser.add_argument("--dense-helper", type=Path)
    args = parser.parse_args()

    # APFS eagerly allocates small truncations even when their unwritten tail
    # reads as zero. A 32 MiB image exposes the physical-allocation difference
    # while remaining far cheaper than the production 512 MiB disk.
    mkfs.TOTAL_BLOCKS = 8192
    mkfs.INODE_COUNT = 128
    mkfs.LOG_BLOCKS = 16
    with tempfile.TemporaryDirectory(prefix="logit-snapshot-sparse-") as tmp:
        directory = Path(tmp)
        source = directory / "source.img"
        output = directory / "snapshot.img"
        builder = mkfs.Builder()
        builder.add_file("/browser/state", b"old-state")
        raw, used = builder.serialize()
        source.write_bytes(raw)
        source_hash = hashlib.sha256(raw).digest()
        physical = snapshot(args.helper, source, output)
        check(output.read_bytes() == raw,
              "unrecovered snapshot differs from input raw bytes")
        check(hashlib.sha256(source.read_bytes()).digest() == source_hash,
              "snapshot modified its input")

        # The helper replays the journal into MAP_PRIVATE memory. A shortcut
        # based on source-file hole ranges would miss those changed pages.
        replacement = committed_journal(source)
        source_hash = hashlib.sha256(source.read_bytes()).digest()
        recovered = directory / "recovered.img"
        replay_physical = snapshot(args.helper, source, recovered)
        image = CheckedImage(recovered)
        check(image.payload(image.resolve("/browser/state")) == replacement,
              "sparse snapshot lost a committed journal write")
        check(hashlib.sha256(source.read_bytes()).digest() == source_hash,
              "journal recovery modified its input")
        if args.dense_helper:
            dense = directory / "dense-recovered.img"
            snapshot(args.dense_helper, source, dense)
            check(recovered.read_bytes() == dense.read_bytes(),
                  "sparse journal snapshot differs from the dense raw image")

        logical = source.stat().st_size
        check(physical < logical // 4 and replay_physical < logical // 4,
              "snapshot remains dense: physical=%d/%d, logical=%d, used=%d"
              % (physical, replay_physical, logical, used))
        print("snapshot-sparse: raw bytes, journal replay, input and mode preserved; "
              "physical=%d/%d of logical=%d" % (physical, replay_physical, logical))


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        sys.exit("FAIL: " + str(error))
