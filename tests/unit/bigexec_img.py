#!/usr/bin/env python3
"""Build a LogitFS image that is the Makefile's disk PLUS some extra files.

WHY THIS EXISTS AND WHY IT IS NOT A MAKEFILE EDIT. The file list for
build/disk.img lives in one enormous mkfs.py invocation in the top-level
Makefile, and that file is contended -- several workflows are in it at once. So
this asks `make -n` what the recipe IS and appends to its argv, rather than
retyping the list here. A retyped list is a second thing to keep in step, and it
goes stale silently: the image still builds and is merely missing whatever was
added last.

AND THE CONTINUATIONS ARE JOINED FIRST. CLAUDE.md's rule ("If you write anything
that reads the Makefile, join the continuations first") applies to `make -n`
output too, and that IS a surprise -- make echoes the recipe with its
backslash-newlines intact, because it hands the whole thing to one shell. The
mkfs line is 54 tokens on its first physical line and 300-odd in total, so a
reader that takes the first line gets an image with about a fifth of its files,
no error, and a boot that fails somewhere else entirely.

tools/mkfs.py is IMPORTED, never re-implemented: it produces every byte of the
filesystem and there is no second definition of the format anywhere. (Shape
taken from a scratch tool an earlier session wrote for the same reason.)

usage: bigexec_img.py <repo> <make-n-file> <out.img> [host:/guest ...]
"""
import importlib.util
import os
import re
import sys

repo, mkn, out = sys.argv[1], sys.argv[2], sys.argv[3]
extras = sys.argv[4:]

text = open(mkn, encoding="utf-8", errors="replace").read()
text = re.sub(r"\\\n[ \t]*", " ", text)          # <-- the whole point
lines = [l for l in text.split("\n") if "tools/mkfs.py" in l]
if len(lines) != 1:
    sys.exit("bigexec_img: expected exactly one mkfs.py line in `make -n`, got %d"
             % len(lines))
argv = lines[0].split()
# argv[0] is python3, argv[1] tools/mkfs.py, argv[2] the image path.
specs = argv[3:]
if any(a.endswith("\\") for a in specs):
    sys.exit("bigexec_img: a spec still ends in a backslash -- the join failed")

missing = [s.split(":")[0] for s in specs + extras if not os.path.exists(s.split(":")[0])]
if missing:
    sys.exit("bigexec_img: %d input(s) do not exist, e.g. %s" % (len(missing), missing[:3]))

spec = importlib.util.spec_from_file_location(
    "mkfs", os.path.join(repo, "tools", "mkfs.py"))
mkfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkfs)

b = mkfs.Builder()
for s in specs + extras:
    host, guest = s.split(":", 1) if ":" in s else (s, "/" + os.path.basename(s))
    b.add_file(guest, open(host, "rb").read())
img, used = b.serialize()
with open(out, "wb") as f:
    f.write(img)
print("bigexec_img: %s -- %d files (%d from the Makefile, %d extra), "
      "%d of %d blocks, %d of %d inodes"
      % (out, len(specs) + len(extras), len(specs), len(extras),
         used, mkfs.TOTAL_BLOCKS, b.next_ino, mkfs.INODE_COUNT))
