#!/usr/bin/env python3
"""Run the cross-core heap ownership gate and its watched-red old behavior."""
from pathlib import Path
import argparse
import subprocess


ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("--build", required=True, type=Path)
args = parser.parse_args()
out = args.build.resolve()
out.mkdir(parents=True, exist_ok=True)

sources = [
    "tests/unit/kheap_crosscore_test.c",
    "c/kernel/mm/phys/kheap.c",
    "c/kernel/cpu/spinlock.c",
]
flags = [
    "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
    "-pthread", "-DMM_HOSTTEST", "-DMM_CONCURRENT", "-DLOGIT_LOCK_HOST",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    "-Itests/unit", "-Itests/unit/mmstub", "-Ic/kernel/mm",
    "-Ic/kernel/mm/phys", "-Ic/kernel/mm/virt", "-Ic/kernel/mm/cache",
    "-Ic/kernel/mm/reclaim",
]


def run_case(name: str, control: bool) -> subprocess.CompletedProcess[str]:
    binary = out / name
    cmd = flags + (["-DKHEAP_NO_CROSSCORE_CLAIM"] if control else [])
    built = subprocess.run(cmd + sources + ["-o", str(binary)], cwd=ROOT,
                           text=True, capture_output=True)
    (out / f"{name}-build.log").write_text(built.stdout + built.stderr)
    if built.returncode:
        raise SystemExit(f"{name}: compile failed; see {out / f'{name}-build.log'}")
    result = subprocess.run([str(binary)], cwd=ROOT, text=True,
                            capture_output=True, timeout=30)
    (out / f"{name}.log").write_text(result.stdout + result.stderr)
    return result


control = run_case("without-claim", True)
old_failures = (
    "FAIL cross-core duplicate free is refused",
    "FAIL cross-core double free never returns one block to two owners",
)
if control.returncode != 1 or any(line not in control.stdout for line in old_failures):
    raise SystemExit("control did not expose cross-core duplicate ownership: "
                     + control.stdout + control.stderr)
print(control.stdout, end="")
print("cross-core control: PASS (old per-CPU-only check rejected)")

fixed = run_case("with-claim", False)
if fixed.returncode or "KHEAP_CROSSCORE warnings=3 failures=0" not in fixed.stdout:
    raise SystemExit("cross-core heap gate failed: " + fixed.stdout + fixed.stderr)
print(fixed.stdout, end="")
print("cross-core heap: PASS")
