#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The sysroot is ON the image, byte for byte.

Walk build/sysroot on the host; for every regular file, resolve the same path
on the image and compare the bytes. Never a length check (CLAUDE.md, Storage:
a filesystem that hands one block to two files produces a file of exactly the
right length holding someone else's data). Also counts what the image carries
under /usr so the fragment can report it, and checks the execute bit was NOT
set on anything under /usr/include or /usr/lib (they are not programs; the
destination rule in mkfs.py must leave them 0644-by-default).
The SDK manifest names the only programs allowed to carry execute bits; a
new file under /bin is not automatically trusted just because of its path.

usage: sysroot_img_test.py <image> <sysroot-dir>
"""
import importlib.util
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
spec = importlib.util.spec_from_file_location(
    "lfs_extract", os.path.join(REPO, "tests", "boot", "lfs_extract.py"))
lfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lfs)

OFF_XMODE = 88          # tools/mkfs.py OFF_XMODE; MODE_SET = 0x8000


def main():
    img_path, root = sys.argv[1], os.path.abspath(sys.argv[2])
    manifest = os.path.join(root, "usr/share/openlogit/manifest.json")
    with open(manifest, encoding="utf-8") as f:
        installed = json.load(f)["files"]
    programs = {"/" + entry["path"] for entry in installed
                if entry["path"].startswith("bin/")}
    with open(img_path, "rb") as f:
        img = bytearray(f.read())
    g = lfs.sb(img)
    n = nbytes = bad = mode_errors = 0
    seen_programs = set()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            host = os.path.join(dirpath, fn)
            rel = "/" + os.path.relpath(host, root).replace(os.sep, "/")
            ino = lfs.resolve(img, g, rel)
            if lfs.itype(img, g, ino) != lfs.T_FILE:
                print("  FAIL %s is not a regular file on the image" % rel)
                bad += 1
                continue
            blks, size = lfs.blocks_of(img, g, ino)
            data = b"".join(bytes(img[b * lfs.BS:(b + 1) * lfs.BS]) for b in blks)[:size]
            want = open(host, "rb").read()
            if data != want:
                print("  FAIL %s differs (image %d B, host %d B)" % (rel, len(data), len(want)))
                bad += 1
            xmode = struct.unpack_from("<I", img, lfs.ino_off(g, ino) + OFF_XMODE)[0]
            executable = bool(xmode & 0o111)
            if rel in programs:
                seen_programs.add(rel)
                if not executable:
                    mode_errors += 1
                    print("  FAIL %s is an SDK program without an execute bit" % rel)
            elif executable:
                mode_errors += 1
                print("  FAIL %s carries an unexpected execute bit (%o)" % (rel, xmode & 0o7777))
            n += 1
            nbytes += len(want)
    missing = programs - seen_programs
    if missing:
        print("  FAIL SDK manifest names programs missing from the sysroot: %s" % sorted(missing))
        mode_errors += len(missing)
    print("sysroot_img_test: %d files under %s compared byte-for-byte, %d bytes, %d mismatches, "
          "%d execute-bit errors" % (n, os.path.basename(root), nbytes, bad, mode_errors))
    sys.exit(1 if (bad or mode_errors or n == 0) else 0)


if __name__ == "__main__":
    main()
