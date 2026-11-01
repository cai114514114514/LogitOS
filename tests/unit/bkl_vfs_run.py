#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real VFS pthread race tests. Faulty source variants must fail named checks."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    """Build the current VFS layout and require each mutation's intended failure.

    The old flat c/fs paths stopped compiling after the VFS/ramfs split. Keep
    the real implementation and assertions; only its source/include locations
    change. Include the VFS directory for the copied mutant's relative headers
    too, otherwise the control fails to compile instead of testing ownership.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--negative-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    source = (root / "c/fs/vfs/vfs.c").read_text()
    variants = ["no-owner", "no-refs"] if args.negative_only else ["positive"]
    for name in variants:
        text = source
        if name == "no-owner":
            old = "if (ref.m && owned) { io_domain_enter(&ref.m->op); ref.owned = 1; }"
            assert text.count(old) == 1, "transaction owner anchor changed"
            text = text.replace(old, "(void)owned; /* injected missing transaction owner */")
        if name == "no-refs":
            old = "__atomic_fetch_add(&ref.m->refs, 1, __ATOMIC_RELAXED)"
            release = "__atomic_fetch_sub(&ref->m->refs, 1, __ATOMIC_RELEASE)"
            assert text.count(old) == 2, "mount borrower anchors changed"
            assert text.count(release) == 1, "mount release anchor changed"
            text = text.replace(old, "(void)0").replace(release, "(void)0")
        variant = build / (name + ".c")
        variant.write_text(text)
        binary = build / name
        command = [os.environ.get("CC", "clang"), "-O1", "-g", "-Wall", "-Wextra",
                   "-pthread", "-fsanitize=address,undefined"]
        # vfs_path.h now derives its limits from the real LogitFS format.
        for directory in ("tests/unit", "c/fs/vfs", "c/fs/ramfs", "c/fs/ctl",
                          "c/fs/logitfs", "c/kernel/core"):
            command.append("-I" + str(root / directory))
        command.extend([
            str(root / "tests/unit/bkl_vfs_test.c"), str(variant),
            str(root / "c/fs/vfs/vfs_meta.c"), str(root / "c/fs/vfs/vfs_path.c"),
            str(root / "c/fs/ramfs/ramfs.c"), "-o", str(binary),
        ])
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        (build / (name + ".log")).write_text(result.stdout + result.stderr)
        if name == "positive":
            assert result.returncode == 0, (result.stdout, result.stderr)
        else:
            expected = {
                "no-owner": "8000 names survive backend",
                "no-refs": "unmount must wait for active mount borrower",
            }[name]
            assert result.returncode == 1 and "FAIL: " + expected in result.stdout, (
                name, result.stdout, result.stderr)
            print(name + ": observed FAIL: " + expected)
        print(name + ": " + result.stdout.strip().splitlines()[-1])


if __name__ == "__main__":
    main()
