#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check idle sysroot decisions against the real product Makefile."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[3]
MAKE = shutil.which("gmake") or shutil.which("make")


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def make(build, target, *, mode=None, flags=None, overlay=None):
    command = [MAKE, "-f", "Makefile"]
    if overlay:
        command += ["-f", str(overlay)]
    if mode:
        command.append(mode)
    command.append(f"BUILD={build}")
    if flags is not None:
        command.append(f"SYSROOT_MKFLAGS={flags}")
    command.append(str(target))
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    require(result.returncode in (0, 1),
            f"make failed ({result.returncode}): {result.stderr[-1200:]}")
    return result


def fixture(directory):
    build = directory / "build"
    flags = build / "sysroot-work/mkflags.txt"
    require(make(build, flags).returncode == 0, "could not create sysroot flags")

    # The snapshot copies each source before touching its stamp. An empty
    # source recipe with a normal stamp edge used to look dirty to -q/-n even
    # when the object was newer than both files. No compiler is needed here:
    # -q must settle the real graph using only their timestamps.
    source = build / "sysroot-work/tccsrc/lib/libtcc1.c"
    stamp = build / "sysroot-work/tccsrc.stamp"
    obj = build / "sysroot-work/tcc1/libtcc1.o"
    source.parent.mkdir(parents=True)
    obj.parent.mkdir(parents=True)
    source.write_text("int fixture(void) { return 1; }\n")
    stamp.touch()
    obj.write_bytes(b"existing object")

    live = ROOT / "third_party/tcc"
    inputs = [ROOT / "tests/unit/sysroot_tccsnap.sh", ROOT / "tests/sysroot.mk"]
    for pattern in ("*.c", "*.h", "include/*.h", "lib/*"):
        inputs.extend(live.glob(pattern))
    latest = max(path.stat().st_mtime_ns for path in inputs if path.is_file())
    # Make 3.81 compares whole seconds. Space the three mtimes by full
    # seconds while keeping the stamp newer than every real prerequisite.
    # A fresh checkout can put the private fixture slightly in the future;
    # the query only reads it and never installs it into the product image.
    source_time = max(time.time_ns() - 2_000_000_000, latest + 1)
    for path, mtime in ((source, source_time),
                        (stamp, source_time + 1_000_000_000),
                        (obj, source_time + 2_000_000_000)):
        os.utime(path, ns=(mtime, mtime))
    return build, flags, source, obj


def check_positive(directory):
    build, flags, source, obj = fixture(directory)
    require(make(build, flags, mode="-q").returncode == 0,
            "idle sysroot flags are reported dirty")
    require(make(build, obj, mode="-q").returncode == 0,
            "idle snapshot object is reported dirty")
    require("mkflags.txt.tmp" not in make(build, flags, mode="-n").stdout,
            "idle dry-run predicts a sysroot flag rewrite")
    require(str(source) not in make(build, obj, mode="-n").stdout,
            "idle dry-run predicts a snapshot compile")

    before = (flags.read_bytes(), flags.stat().st_mtime_ns)
    require(make(build, flags, mode="-q", flags="--without-libtcc1").returncode == 1,
            "changed SYSROOT_MKFLAGS did not invalidate the flags file")
    require("mkflags.txt.tmp" in make(build, flags, mode="-n",
                                       flags="--without-libtcc1").stdout,
            "changed SYSROOT_MKFLAGS is missing from the dry-run")
    require((flags.read_bytes(), flags.stat().st_mtime_ns) == before,
            "query or dry-run wrote sysroot flags")
    print("sysroot idle: unchanged -q=0, changed flags -q=1, dry-run truthful")


def check_control(directory):
    build, flags, source, obj = fixture(directory)
    require(make(build, flags, mode="-q").returncode == 0,
            "baseline flags must be idle before the control")
    require(make(build, obj, mode="-q").returncode == 0,
            "baseline object must be idle before the control")

    # Restore the two old dependency shapes as overlays. Each must make the
    # original false-positive query fail for its own target.
    phony = directory / "phony.mk"
    phony.write_text(".PHONY: old-sysroot-flag-check\n"
                     "$(SYSROOT_WORK)/mkflags.txt: old-sysroot-flag-check\n")
    require(make(build, flags, mode="-q", overlay=phony).returncode == 1,
            "negative control: old phony edge did not dirty mkflags.txt")
    require("mkflags.txt.tmp" in make(build, flags, mode="-n", overlay=phony).stdout,
            "negative control: old phony edge did not predict the old rewrite")

    source_edge = directory / "source-edge.mk"
    source_edge.write_text("$(SYSROOT_TCCSRC)/lib/libtcc1.c: $(SYSROOT_TCC_STAMP)\n")
    require(make(build, obj, mode="-q", overlay=source_edge).returncode == 1,
            "negative control: old source edge did not dirty libtcc1.o")
    require(str(source) in make(build, obj, mode="-n", overlay=source_edge).stdout,
            "negative control: old source edge did not predict a compile")
    print("sysroot idle controls: phony flag and normal source edges went red")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", action="store_true")
    args = parser.parse_args()
    require(MAKE is not None, "GNU Make is required")
    with tempfile.TemporaryDirectory(prefix="logitos-sysroot-idle-") as temporary:
        directory = Path(temporary)
        if args.control:
            check_control(directory)
        else:
            check_positive(directory)


if __name__ == "__main__":
    main()
