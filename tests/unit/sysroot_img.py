#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the sysroot TEST image: the Makefile's disk plus build/sysroot/ packed
at / with mkfs.py's add_tree, plus any extra host:/guest pairs.

The file list is TAKEN FROM `make -n`, not retyped (the same reason
tests/unit/bigexec_img.py and build/scr/bigdisk.py give: a second list goes
stale silently). The shared disk-recipe reader joins continuations and skips
leading preservation options. Taking argv[3:] instead silently treated
--preserve and its paths as files when the product recipe gained those options.

tools/mkfs.py is imported and its own add_spec() does every file and the
tree, so there is one definition of the format and one of the spec syntax.

Prints the numbers the fragment reports: files, inodes used of INODE_COUNT,
blocks used, and the bytes the sysroot added.

usage: sysroot_img.py <repo> <make-n-file> <out.img> <spec ...>
"""
import importlib.util
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "boot"))
from make_disk_recipe import disk_recipe

repo, mkn, out = sys.argv[1], sys.argv[2], sys.argv[3]
extras = sys.argv[4:]

text = open(mkn, encoding="utf-8", errors="replace").read()
_, specs = disk_recipe(text)

spec = importlib.util.spec_from_file_location("mkfs", os.path.join(repo, "tools", "mkfs.py"))
mkfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkfs)

missing = [mkfs.parse_spec(s)[0] for s in specs + extras
           if not os.path.exists(mkfs.parse_spec(s)[0])]
if missing:
    sys.exit("sysroot_img: %d input(s) do not exist, e.g. %s" % (len(missing), missing[:3]))

b = mkfs.Builder()
base_files = 0
for s in specs:
    base_files += mkfs.add_spec(b, s)[0]
base_inodes = b.next_ino
extra_files = 0
for s in extras:
    extra_files += mkfs.add_spec(b, s)[0]
# New directories = new inodes that are not files (add_tree reports the
# directories it WALKED, which includes ones the base image already had).
extra_dirs = (b.next_ino - base_inodes) - extra_files
img, used = b.serialize()
with open(out, "wb") as f:
    f.write(img)
print("sysroot_img: %s" % out)
print("  Makefile's list: %d files, %d inodes" % (base_files, base_inodes))
print("  added:           %d files + %d new dirs from %s" % (extra_files, extra_dirs, " ".join(extras)))
print("  inodes:          %d of %d used (%d free)" % (b.next_ino, mkfs.INODE_COUNT, mkfs.INODE_COUNT - b.next_ino))
print("  blocks:          %d of %d used = %d bytes (%.1f MiB)"
      % (used, mkfs.TOTAL_BLOCKS, used * mkfs.BS, used * mkfs.BS / 1048576))
