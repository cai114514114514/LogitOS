#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Serialize ordinary disk rebuild/launch and refuse already-open images.

The companion lock closes races between make's packer and launcher. The host
open-file check also catches QEMU processes started before that lock existed,
or launched directly. No file bytes are inspected and no process is stopped.
"""
import contextlib
import fcntl
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys


def assert_unused(path):
    path = Path(path)
    if not path.exists():
        return
    lsof = shutil.which("lsof")
    if not lsof:
        raise RuntimeError("lsof is required to verify that the disk is not open")
    result = subprocess.run([lsof, "-t", "--", str(path)], capture_output=True, text=True)
    owners = sorted({int(s) for s in result.stdout.split() if s.isdecimal()})
    if owners:
        raise RuntimeError("disk is open by PID(s) " + ",".join(map(str, owners)) +
                           "; close that VM before rebuilding or launching this disk")
    if result.returncode not in (0, 1) or result.stderr.strip():
        raise RuntimeError("could not verify disk ownership; refusing to replace it")


@contextlib.contextmanager
def image_guard(path):
    path = Path(path).absolute()
    if path.is_symlink():
        raise RuntimeError("refusing a symlink disk destination")
    if path.exists() and not path.is_file():
        raise RuntimeError("refusing a non-regular disk destination")
    lock = os.open(str(path) + ".lock", os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        if not stat.S_ISREG(os.fstat(lock).st_mode):
            raise RuntimeError("disk lock is not a regular file")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("disk is already held by a builder or running VM") from None
        assert_unused(path)
        yield lock
    finally:
        os.close(lock)


def main():
    if len(sys.argv) < 4 or sys.argv[2] != "--":
        raise RuntimeError("usage: disk_guard.py <disk.img> -- <qemu> [arguments...]")
    with image_guard(sys.argv[1]) as lock:
        # Inheritance retains the advisory lock if the launcher is interrupted
        # while QEMU is still shutting down; lsof remains the external backstop.
        return subprocess.call(sys.argv[3:], pass_fds=(lock,))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as exc:
        sys.exit("disk guard: " + str(exc))
