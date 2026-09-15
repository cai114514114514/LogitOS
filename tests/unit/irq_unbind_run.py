#!/usr/bin/env python3
"""Verify that failed hardware IRQ release blocks device removal."""
from pathlib import Path
import argparse
import os
import subprocess


parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--negative-only", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
variants = [
    ("LOGIT_X2APIC_NEGCTL_UNBIND_AFTER_IRQ_RELEASE_FAIL",
     "failed IRQ release must keep drvdata and skip remove")
] if args.negative_only else [("", None)]

for macro, expected in variants:
    out = args.build.resolve() / (macro.lower() if macro else "positive")
    out.mkdir(parents=True, exist_ok=True)
    binary = out / "devmodel_test"
    command = [
        os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-DLOGIT_HOST_TEST", "-Itests/unit/pcistub", "-Ic/drivers/core",
        "-Ic/kernel/pci", "tests/unit/devmodel_test.c",
        "c/drivers/core/device.c", "-o", str(binary),
    ]
    if macro:
        command.insert(1, "-D" + macro)
    subprocess.run(command, cwd=root, check=True)
    result = subprocess.run([str(binary)], cwd=root, capture_output=True,
                            text=True, timeout=20)
    (out / "result.log").write_text(result.stdout + result.stderr)
    # Keep the exact failing assertion observable: an unrelated sanitizer or
    # second failure must never satisfy this control.
    failed = [line for line in result.stdout.splitlines()
              if line.startswith("FAIL: ")]
    if expected:
        assert result.returncode == 1, (result.stdout, result.stderr)
        assert failed == ["FAIL: " + expected], (failed, result.stderr)
    else:
        assert result.returncode == 0 and not failed, (result.stdout, result.stderr)
    print((macro or "positive") + ": " + result.stdout.strip().splitlines()[-1])
