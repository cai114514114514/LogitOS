#!/usr/bin/env python3
"""Build the production UEFI range policy and its source-level mutation."""
from pathlib import Path
import argparse
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--mode", choices=("positive", "negctl"), required=True)
args = parser.parse_args()

root = Path(__file__).resolve().parents[2]
build = args.build.resolve()
build.mkdir(parents=True, exist_ok=True)
test_source = root / "tests/unit/efi_load_policy_test.c"
include_dir = root / "c/boot/efi"

if args.mode == "negctl":
    include_dir = build / "ignore-reserved-overlap"
    include_dir.mkdir(parents=True, exist_ok=True)
    source = (root / "c/boot/efi/load_policy.h").read_text()
    needle = "if (d->Type != EfiLoaderData) return 0;"
    if source.count(needle) != 1:
        raise SystemExit("mutation anchor drifted: expected exactly one type check")
    (include_dir / "load_policy.h").write_text(
        source.replace(needle, "if (0) return 0; /* NEGCTL: ignore reserved overlap */"))
    shutil.copyfile(root / "c/boot/efi/efi.h", include_dir / "efi.h")

exe = build / args.mode
command = ["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
           "-I" + str(include_dir), str(test_source), "-o", str(exe)]
subprocess.run(command, check=True)
run = subprocess.run([str(exe)], text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE)
(build / (args.mode + ".log")).write_text(run.stdout + run.stderr)

if args.mode == "positive":
    print(run.stdout, end="")
    print(run.stderr, end="")
    if run.returncode:
        raise SystemExit(run.returncode)
else:
    witness = "FAIL: ACPI NVS overlap rejected"
    if run.returncode == 0:
        raise SystemExit("CONTROL FAILED: ignoring reserved overlap stayed green")
    if witness not in run.stderr:
        raise SystemExit("control failed for the wrong reason:\n" + run.stdout + run.stderr)
    print("CONTROL CAUGHT ignore-reserved-overlap: " + run.stderr.strip())
