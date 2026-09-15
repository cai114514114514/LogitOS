#!/usr/bin/env python3
"""Real-MM wide address space regression and observable legacy-only fork control.

The source list is taken from mm_run.sh rather than copied: new MM dependencies
must reach both suites. The host arena stays small; only virtual reservations
are large. A control must compile and print the specific lost-high-leaf failure,
so an ASan startup error or a generic exit failure cannot count as evidence.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=Path, required=True)
    args = ap.parse_args()
    out = args.build.resolve()
    out.mkdir(parents=True, exist_ok=True)
    mm = root / "c/kernel/mm"
    runner = (root / "tests/unit/mm_run.sh").read_text()
    source_line = re.search(r'^MMSRC="([^"]+)"', runner, re.M)
    if not source_line:
        raise SystemExit("Cannot locate authoritative MM source list in mm_run.sh")
    src = shlex.split(source_line[1].replace("$MM", str(mm)))
    # c/kernel/mm was split into subdirectories on 2026-09-15 and this list is the
    # gate's whole include path -- INCDIRS does not reach here on purpose.
    inc = [root / "tests/unit", root / "tests/unit/mmstub", mm,
           mm / "phys", mm / "virt", mm / "cache", mm / "reclaim"]
    flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-DMM_HOSTTEST",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    cc = shlex.split(os.environ.get("CC", "cc"))
    common = [str(root / "tests/unit/wideva_test.c"), str(root / "tests/unit/mm_common.c"),
              *src, str(root / "tests/unit/mmstub/mm_hoststub.c")]
    for control in (False, True):
        name = "wideva_skip_clone" if control else "wideva_test"
        binary = out / name
        cmd = cc + flags + ["-I" + str(p) for p in inc]
        if control:
            cmd += ["-DWIDEVA_SKIP_CLONE"]
        subprocess.run(cmd + common + ["-o", str(binary)], check=True, cwd=root)
        run = subprocess.run([str(binary)], cwd=root, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60)
        (out / (name + ".log")).write_text(run.stdout)
        print(run.stdout, end="")
        if control:
            if run.returncode != 1 or "FAIL: wide fork preserves" not in run.stdout:
                raise SystemExit("Control did not fail on a lost high-address fork mapping")
            print("WIDEVA CONTROL PASS: legacy-only clone loses high leaves as expected")
        elif run.returncode:
            raise SystemExit(run.returncode)
    print("WIDEVA PASS: production and negative control verified")


if __name__ == "__main__":
    main()
