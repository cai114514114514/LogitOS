#!/usr/bin/env python3
"""Pack the Makefile's disk image PLUS the files tests/tcc.mk puts under test.

WHY A SEPARATE IMAGE. The $(DISK) rule's file list is contended -- owned by
the Makefile's line and edited by the sysroot line as this is written -- and a
gate that adds its fixtures to it would be a gate that edits a file it does
not own on every run. So this IMPORTS tools/mkfs.py as a module and calls its
own Builder: mkfs.py produces every byte of the filesystem, there is no second
definition of the format, and the execute bit is set by mkfs's own rule -- by
DESTINATION (/bin, /sbin, *.aex), which is why the bare ELFs go under /bin and
nothing here touches xmode.

THE FILE LIST IS TAKEN FROM `make -n`, NOT RE-DERIVED, for the reason
build/scr/bigdisk.py gave first: a retyped list goes stale silently. And the
continuations are joined before anything else (CLAUDE.md's rule), because make
echoes the recipe with its backslash-newlines intact -- the mkfs invocation is
54 tokens on its first physical line and 300-odd in total.

usage: mk-tcc-disk.py <repo> <make-n-file> <out.img> [host:/dest ...]
"""
import importlib.util
import os
import re
import sys

repo, mkn, out = sys.argv[1], sys.argv[2], sys.argv[3]
extras = sys.argv[4:]

text = open(mkn, encoding="utf-8", errors="replace").read()
text = re.sub(r"\\\r?\n[ \t]*", " ", text)       # join continuations FIRST
lines = [l for l in text.split("\n") if "tools/mkfs.py" in l]
if len(lines) != 1:
    sys.exit("mk-tcc-disk: expected exactly one mkfs.py line in %s, got %d"
             % (mkn, len(lines)))
argv = lines[0].split()
# argv[0] python3, argv[1] tools/mkfs.py, argv[2] the image path, then specs.
specs = argv[3:]
if any(a.endswith("\\") for a in specs):
    sys.exit("mk-tcc-disk: a spec still ends in a backslash -- the join failed")

missing = [s.split(":")[0] for s in specs + extras
           if not os.path.exists(s.split(":")[0])]
if missing:
    sys.exit("mk-tcc-disk: %d input(s) do not exist, e.g. %s" % (len(missing), missing[:3]))

spec = importlib.util.spec_from_file_location("mkfs", os.path.join(repo, "tools", "mkfs.py"))
mkfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkfs)

b = mkfs.Builder()
n = 0
# mkfs.py's own spec parser, not a copy of it: a spec can name a DIRECTORY
# (fsroot/ime is one) and add_spec() knows that and this file does not.
for s in specs + extras:
    n += mkfs.add_spec(b, s)[0]
img, used = b.serialize()
with open(out, "wb") as f:
    f.write(img)
print("mk-tcc-disk: %d files (%d from the Makefile, %d extra); %d of %d blocks, %d of %d inodes -> %s"
      % (n, len(specs), len(extras), used, mkfs.TOTAL_BLOCKS, b.next_ino, mkfs.INODE_COUNT, out))
for s in extras:
    host, guest = s.split(":", 1)
    print("    %-18s %9d bytes  %s" % (guest, os.path.getsize(host),
          "exec" if mkfs.Builder.is_program(guest) else "data"))
