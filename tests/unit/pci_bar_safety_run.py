#!/usr/bin/env python3
"""Run production PCI BAR ownership checks and five observed regressions."""
import argparse
import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PARSER = argparse.ArgumentParser()
PARSER.add_argument("--build", required=True, type=Path)
ARGS = PARSER.parse_args()
OUT = ARGS.build.resolve()
OUT.mkdir(parents=True, exist_ok=True)

BASE = [
    os.environ.get("CC", "clang"),
    "-O1", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra",
    "-Werror", "-DLOGIT_HOST_TEST",
    "-I" + str(ROOT / "c/drivers/core"),
    "-I" + str(ROOT / "c/kernel/pci"),
    "-I" + str(ROOT / "tests/unit/pcistub"),
]
SOURCES = [
    str(ROOT / "tests/unit/pci_test.c"),
    str(ROOT / "c/kernel/pci/pci.c"),
    str(ROOT / "c/drivers/core/device.c"),
]


def build_run(name, mode, macro=None, expected_failure=None):
    exe = OUT / name
    cmd = BASE[:]
    if macro:
        cmd.append("-D" + macro)
    subprocess.run(cmd + ["-o", str(exe)] + SOURCES, check=True)
    run = subprocess.run([str(exe), mode], capture_output=True, text=True,
                         timeout=20)
    output = run.stdout + run.stderr
    (OUT / (name + ".log")).write_text(output)
    print(output, end="")
    if "runtime error:" in output or "AddressSanitizer" in output:
        raise SystemExit(name + ": sanitizer failure")
    if expected_failure:
        if (run.returncode != 1 or output.count("FAIL:") != 1 or
                expected_failure not in output):
            raise SystemExit(name + ": control did not fail exactly once")
        print("negative control OK:", name)
    elif run.returncode:
        raise SystemExit(name + ": positive fixture failed")


build_run("positive", "safety")
build_run(
    "neg-decode-readback", "decode",
    "PCI_BAR_NEGCTL_SKIP_DECODE_READBACK",
    "FAIL: ignored decode-disable write is refused before BAR sizing",
)
build_run(
    "neg-partial-decode-recovery", "partial-decode",
    "PCI_BAR_NEGCTL_SKIP_PARTIAL_COMMAND_RECOVERY",
    "FAIL: partial decode-disable refusal restores complete PCI Command",
)
build_run(
    "neg-bar-restore-readback", "bar-restore",
    "PCI_BAR_NEGCTL_SKIP_BAR_RESTORE_READBACK",
    "FAIL: failed BAR restore leaves decode off and publishes no resource",
)
build_run(
    "neg-bar-high-restore-readback", "bar-high-restore",
    "PCI_BAR_NEGCTL_SKIP_BAR_RESTORE_READBACK",
    "FAIL: failed 64-bit BAR high restore leaves decode off and publishes no resource",
)
build_run(
    "neg-command-restore-readback", "command-restore",
    "PCI_BAR_NEGCTL_SKIP_COMMAND_RESTORE_READBACK",
    "FAIL: failed Command restore leaves valid BAR decoded off and unpublished",
)
print("PCI BAR safety: positive plus 5 observed negative controls PASS")
