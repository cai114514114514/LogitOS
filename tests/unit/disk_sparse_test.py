#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The normal mkfs path must produce the same bytes with fewer host blocks."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import mkfs
from disk_profile import CheckedImage, atomic_write


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dense-control", action="store_true")
    args = parser.parse_args()

    # 32 MiB exercises the real format without allocating two 512 MiB images.
    # APFS eagerly allocates holes in smaller files, so an 8 MiB fixture would
    # misreport the optimization as absent. The payload crosses indirect blocks.
    mkfs.TOTAL_BLOCKS = 8192
    mkfs.INODE_COUNT = 128
    mkfs.LOG_BLOCKS = 16
    payload = bytes(range(256)) * 320
    with tempfile.TemporaryDirectory(prefix="logit-disk-sparse-") as directory:
        work = Path(directory)
        source = work / "payload.bin"
        dense = work / "dense.img"
        sparse = work / "sparse.img"
        source.write_bytes(payload)

        builder = mkfs.Builder()
        builder.add_file("/payload", payload)
        with patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1735689600"}):
            image, used = builder.serialize()
        atomic_write(dense, image)

        shim = work / "mkfs_fixture.py"
        shim.write_text(
            "import sys\n"
            f"sys.path.insert(0, {str(ROOT / 'tools')!r})\n"
            "import mkfs\n"
            "mkfs.TOTAL_BLOCKS = 8192\n"
            "mkfs.INODE_COUNT = 128\n"
            "mkfs.LOG_BLOCKS = 16\n"
            "mkfs.main()\n"
        )
        env = dict(os.environ, SOURCE_DATE_EPOCH="1735689600")
        result = subprocess.run(
            [sys.executable, str(shim), str(sparse), f"{source}:/payload"],
            capture_output=True, text=True, env=env,
        )
        if result.returncode:
            raise AssertionError("mkfs fixture failed: " + result.stderr)

        # The dense reference and normal mkfs path are built with one fixed
        # timestamp so their full logical bytes can be compared directly.
        reference = dense.read_bytes()
        candidate = (dense if args.dense_control else sparse).read_bytes()
        if candidate != reference or len(candidate) != mkfs.TOTAL_BLOCKS * mkfs.BS:
            raise AssertionError("sparse image changed the logical disk bytes")
        checked = CheckedImage(sparse)
        if checked.payload(checked.resolve("/payload")) != payload:
            raise AssertionError("mkfs image lost an indirect payload block")

        prefix = used * mkfs.BS
        dense_bytes = dense.stat().st_blocks * 512
        candidate_bytes = (dense if args.dense_control else sparse).stat().st_blocks * 512
        if prefix >= len(image) // 2 or candidate_bytes >= dense_bytes // 2:
            print(f"FAIL: sparse disk still allocated the zero tail "
                  f"(used={prefix}, dense={dense_bytes}, candidate={candidate_bytes})")
            return 1
        print(f"PASS disk sparse: logical={len(image)} used={prefix} "
              f"dense={dense_bytes} sparse={candidate_bytes}; identical bytes")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        sys.exit("FAIL: " + str(exc))
