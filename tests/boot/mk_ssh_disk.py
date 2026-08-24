#!/usr/bin/env python3
"""Build a disk image identical to $(DISK) (build/disk.img) but with one more
file: build/sshd.aex at /bin/sshd.

sshd is not in the coreutils APPS list the Makefile packs onto $(DISK) --
that is a one-line Makefile edit outside this line's ownership (c/apps/ssh/**
tests/boot/run-ssh-*.sh are; the Makefile is not -- see the task's final
report for the exact line). Until that line lands, `make test-ssh-os` needs
its OWN disk image, and this is how it gets one without duplicating $(DISK)'s
file list by hand: it asks `make -n build/disk.img` for the exact mkfs.py
invocation make would run, and re-executes it with one more src:/dest pair
appended. A hand-copied file list would drift the moment $(DISK)'s changes
and nobody remembers this script -- that's the c/net/core/route.c "two
literals must agree" shape from CLAUDE.md, avoided by not having a second
literal at all.

Usage: mk_ssh_disk.py <repo-root> <out.img> <sshd.aex>
"""
import subprocess
import sys
import shlex
import os


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    root, out_img, sshd_aex = sys.argv[1], sys.argv[2], sys.argv[3]
    os.chdir(root)

    r = subprocess.run(["make", "-n", "build/disk.img"], capture_output=True, text=True)
    lines = r.stdout.splitlines()

    cmd_lines = []
    found = False
    i = 0
    while i < len(lines):
        if lines[i].startswith("python3 tools/mkfs.py"):
            found = True
            while True:
                line = lines[i]
                cont = line.endswith("\\")
                if cont:
                    line = line[:-1]
                cmd_lines.append(line)
                i += 1
                if not cont:
                    break
            break
        i += 1

    if not found:
        sys.exit("mk_ssh_disk: could not find the mkfs.py recipe in "
                  "`make -n build/disk.img` -- has the disk.img rule's shape "
                  "changed? This script's whole point is not guessing that "
                  "shape, so it refuses rather than falling back to one.")

    tokens = shlex.split(" ".join(cmd_lines))
    assert tokens[0] == "python3" and tokens[1] == "tools/mkfs.py"
    tokens[2] = out_img
    tokens.append("%s:/bin/sshd" % sshd_aex)

    print("mk_ssh_disk: %d tokens, output %s, +1 file (sshd)" % (len(tokens), out_img))
    sys.exit(subprocess.run(tokens).returncode)


if __name__ == "__main__":
    main()
