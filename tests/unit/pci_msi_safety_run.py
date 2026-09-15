#!/usr/bin/env python3
"""Run production PCI message-source readback controls one at a time."""
from pathlib import Path
import argparse
import os
import subprocess


parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--negative-only", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
controls = [
    ("LOGIT_X2APIC_NEGCTL_SKIP_INTX_SOURCE_READBACK",
     "MSI setup requires confirmed legacy-source suppression"),
    ("LOGIT_X2APIC_NEGCTL_SKIP_MSI_SETUP_READBACK",
     "MSI setup requires confirmed capability enable"),
    ("LOGIT_X2APIC_NEGCTL_SKIP_MSIX_SETUP_READBACK",
     "MSI-X setup requires confirmed capability enable"),
    ("LOGIT_X2APIC_NEGCTL_SKIP_MSI_TEARDOWN_READBACK",
     "MSI release preserves vector ownership until disable is confirmed"),
    ("LOGIT_X2APIC_NEGCTL_SKIP_MSIX_TEARDOWN_READBACK",
     "MSI-X release preserves vector ownership until disable is confirmed"),
    ("LOGIT_X2APIC_NEGCTL_TRUST_STALE_SETUP_STATE",
     "setup failure trusts fresh teardown readback over stale snapshot"),
    ("LOGIT_X2APIC_NEGCTL_SKIP_ALTERNATE_MESSAGE_QUIESCE",
     "every IRQ request path first quiesces all message capabilities"),
]
variants = controls if args.negative_only else [("", None)]

for macro, expected in variants:
    out = args.build.resolve() / (macro.lower() if macro else "positive")
    out.mkdir(parents=True, exist_ok=True)
    binary = out / "pci_msi_safety_test"
    command = [
        os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-DLOGIT_HOST_TEST", "-Itests/unit/pcistub", "-Ic/drivers/core",
        "-Ic/kernel/pci", "-Ic/kernel/cpu", "tests/unit/pci_msi_test.c",
        "c/kernel/pci/pci_msi.c", "c/kernel/pci/pci.c",
        "c/drivers/core/device.c", "-o", str(binary),
    ]
    if macro:
        command.insert(1, "-D" + macro)
    subprocess.run(command, cwd=root, check=True)
    result = subprocess.run([str(binary)], cwd=root, capture_output=True,
                            text=True, timeout=20)
    (out / "result.log").write_text(result.stdout + result.stderr)
    failed = [line for line in result.stdout.splitlines()
              if line.startswith("FAIL: ")]
    if expected:
        assert result.returncode == 1, (macro, result.stdout, result.stderr)
        assert failed == ["FAIL: " + expected], (macro, failed, result.stderr)
        assert "1 checks, 1 failed" in result.stdout, result.stdout
    else:
        assert result.returncode == 0 and not failed, (result.stdout, result.stderr)
    print((macro or "positive") + ": " + result.stdout.strip().splitlines()[-1])
