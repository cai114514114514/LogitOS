#!/usr/bin/env python3
"""Compile hda.c itself against a bounded MMIO/PCI/DMA host model."""

from pathlib import Path
import argparse
import os
import platform
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--negative-only", action="store_true")
args = parser.parse_args()
repo = args.repo.resolve()
build = args.build.resolve()
build.mkdir(parents=True, exist_ok=True)

source = (repo / "c/drivers/audio/hda.c").read_text()
source, reads = re.subn(
    r"return \*\(volatile uint(8|16|32)_t\s*\*\)\(([^;]+)\);",
    lambda m: f"return test_read({m[2]}, {int(m[1]) // 8});",
    source,
)
source, writes = re.subn(
    r"\*\(volatile uint(8|16|32)_t\s*\*\)\(([^;]+)\) = v;",
    lambda m: f"test_write({m[2]}, {int(m[1]) // 8}, v);",
    source,
)
if (reads, writes) != (3, 3):
    raise SystemExit(f"HDA MMIO seam drifted: reads={reads}, writes={writes}")

# Production io_lock must mask interrupts and spin with x86 PAUSE.  This host
# model is single-threaded and never exercises an interrupt instruction; using
# that header made the test artificially x86-only.  Keep the lock call sites in
# hda.c, but replace only the included header in the generated test TU.  This
# lets Apple Silicon execute the register/ownership model natively instead of
# depending on Rosetta being installed.
source, locks = re.subn(
    r'#include "\.\./core/io_lock\.h"',
    '#define IO_GUARD(lock) ((void)(lock))\n'
    'typedef unsigned io_lock_t;\n'
    '#define IO_LOCK_INIT 0',
    source,
)
if locks != 1:
    raise SystemExit(f"HDA io-lock host seam drifted: replacements={locks}")

# `pause` is an x86 implementation detail of the bounded waits.  The host test
# controls the modeled clocks and iteration ceilings, so a compiler barrier is
# the faithful portable replacement; changing/removing either bound still
# changes the tested production source around it.
source = source.replace('__asm__ volatile ("pause")',
                        '__asm__ volatile ("" ::: "memory")')

include_dirs = [
    path
    for base in ("c", "include")
    for path in (repo / base).rglob("*")
    if path.is_dir() and "/apps/" not in str(path) and "/third_party/" not in str(path)
]


def compile_and_run(tag: str, macro: str | None, expect_fail: str | None) -> None:
    case = build / tag
    case.mkdir(parents=True, exist_ok=True)
    (case / "hda_driver.inc").write_text(source)
    binary = case / "hda-x79-test"
    command = [
        os.environ.get("CC", "clang"),
        "-std=c11",
        "-O1",
        "-g",
        "-ffunction-sections",
        "-fdata-sections",
        "-DLOGIT_HOST_TEST",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(case),
        "-I" + str(repo / "tests/unit/dma_driver_stub"),
        *["-I" + str(path) for path in include_dirs],
        str(repo / "tests/unit/hda_x79_test.c"),
        "-o",
        str(binary),
    ]
    if macro:
        command.insert(8, "-D" + macro)
    if platform.system() == "Darwin":
        command += ["-Wl,-dead_strip"]
    else:
        command += ["-Wl,--gc-sections"]
    if not macro:
        command += ["-fsanitize=address,undefined"]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
    log = result.stdout + result.stderr
    (case / "output.log").write_text(log)
    if expect_fail is None:
        if result.returncode or not re.search(r"^HDA_X79: \d+ checks, 0 failures$", log, re.M):
            raise SystemExit(f"positive HDA fixture failed:\n{log}")
        print(log.strip())
        return
    failures = re.findall(r"^FAIL: (.+)$", log, re.M)
    if result.returncode != 1 or failures != [expect_fail] or not re.search(
        r"^HDA_X79: \d+ checks, 1 failures$", log, re.M
    ):
        raise SystemExit(f"{tag} did not redden exactly its intended check:\n{log}")
    print(f"HDA_X79_NEGCTL {tag}: exactly one observed failure: {failures[0]}")


controls = [
    (
        "master-before-firmware-quiesce",
        "HDA_X79_NEGCTL_MASTER_BEFORE_QUIESCE",
        "firmware DMA is stopped and read back before CRST and BME",
    ),
    (
        "ignore-stuck-firmware-run",
        "HDA_X79_NEGCTL_IGNORE_STUCK_FIRMWARE_RUN",
        "stuck firmware RUN state blocks CRST and BME",
    ),
    (
        "force-256-rings",
        "HDA_X79_NEGCTL_FORCE_256_RINGS",
        "16-entry-only command ring is negotiated and read back",
    ),
    (
        "accept-gpu-audio",
        "HDA_X79_NEGCTL_ACCEPT_GPU_AUDIO",
        "GTX 1050 display-function HDA is declined before X79 onboard audio",
    ),
    (
        "poison-singleton",
        "HDA_X79_NEGCTL_KEEP_POISONED_INSTANCE",
        "failed HDA probe clears the active instance for a later controller",
    ),
    (
        "free-unacked-dma",
        "HDA_X79_NEGCTL_RELEASE_UNACKED_DMA",
        "unacknowledged controller reset retains hardware-owned DMA",
    ),
    (
        "remove-after-irq-failure",
        "HDA_X79_NEGCTL_REMOVE_AFTER_IRQ_RELEASE_FAIL",
        "failed IRQ teardown retains HDA callbacks and DMA",
    ),
    (
        "isr-early-return-on-remove",
        "HDA_X79_NEGCTL_ISR_EARLY_RETURN_ON_REMOVE",
        "removing ISR acknowledges both stream sources on every failed-release re-entry",
    ),
]

if args.negative_only:
    for item in controls:
        compile_and_run(*item)
else:
    compile_and_run("positive", None, None)
