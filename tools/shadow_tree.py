#!/usr/bin/env python3
"""A shadow of a source tree that a negative control may break freely.

    shadow_tree.py SRC DEST [--copy PATH]... [--drop PATH]...

DEST is rebuilt from nothing: one symlink per entry of SRC, except along each
named PATH, where every parent becomes a real directory of symlinks and the
leaf becomes a real COPY (--copy, so the caller can edit it) or is left out
(--drop). Nothing in SRC is ever written.

Why: a control that sabotages the real tree and restores it afterwards is only
private while nothing else reads the tree. A parallel sweep reads it
constantly -- every make re-parses the Makefile -- and a restore that races
another run's backup leaves a broken file behind (tools/license_audit.py's
ROOT comment has the measured case). Readers that follow symlinks
(os.listdir, os.path.isdir, open) see an ordinary tree.
"""
import os
import shutil
import sys


def expand(src, dest, rel):
    """Make DEST/rel's parent chain real directories whose children are links."""
    parts = rel.split("/")
    s, d = src, dest
    for part in parts[:-1]:
        s, d = os.path.join(s, part), os.path.join(d, part)
        if os.path.islink(d):
            os.unlink(d)
            os.mkdir(d)
            for name in os.listdir(s):
                os.symlink(os.path.join(s, name), os.path.join(d, name))
    return os.path.join(s, parts[-1]), os.path.join(d, parts[-1])


def main(argv):
    if len(argv) < 3:
        sys.exit("usage: shadow_tree.py SRC DEST [--copy PATH]... [--drop PATH]...")
    src, dest = os.path.abspath(argv[1]), os.path.abspath(argv[2])
    ops, i = [], 3
    while i < len(argv):
        if argv[i] not in ("--copy", "--drop") or i + 1 >= len(argv):
            sys.exit("shadow_tree.py: bad argument %r" % argv[i])
        ops.append((argv[i], argv[i + 1].strip("/")))
        i += 2
    if dest == src:
        sys.exit("shadow_tree.py: DEST is SRC -- refusing")
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    for name in os.listdir(src):
        os.symlink(os.path.join(src, name), os.path.join(dest, name))
    for op, rel in ops:
        s, d = expand(src, dest, rel)
        if not os.path.lexists(s):
            sys.exit("shadow_tree.py: %s does not exist in %s -- refusing" % (rel, src))
        os.unlink(d)
        if op == "--copy":
            shutil.copy2(s, d)


if __name__ == "__main__":
    main(sys.argv)
