#!/usr/bin/env python3
"""Build the production APIC/SMP models and require each mutation to go red."""
from pathlib import Path
import argparse
import os
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument("--build", type=Path, required=True)
ap.add_argument("--negative-only", action="store_true")
a = ap.parse_args()

root = Path(__file__).resolve().parents[2]
base = a.build.resolve()
base.mkdir(parents=True, exist_ok=True)
sources = [
    root / "tests/unit/x2apic_test.c",
    root / "c/kernel/cpu/apic_model.c",
    root / "c/kernel/cpu/lapic.c",
    root / "c/kernel/cpu/smp_boot_model.c",
]
common = [
    os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
    "-DLOGIT_LAPIC_HOST", "-DLOGIT_SMP_BOOT_HOST",
    "-DLAPIC_IPI_WAIT_SPINS=4",
    "-I" + str(root / "c/kernel/cpu"),
    "-I" + str(root / "c/kernel/core"),
]
controls = [
    ("LOGIT_X2APIC_NEGCTL_IGNORE_TYPE9",
     "MADT type9 preserves its full APIC ID and ACPI UID"),
    ("LOGIT_X2APIC_NEGCTL_DUPLICATE_CPU",
     "matching type0/type9 records do not start a CPU twice"),
    ("LOGIT_X2APIC_NEGCTL_MMIO_WHEN_EXTD",
     "firmware-active x2APIC selects the MSR backend"),
    ("LOGIT_X2APIC_NEGCTL_TRUNCATE_ICR",
     "x2APIC ICR keeps all 32 destination bits"),
    ("LOGIT_X2APIC_NEGCTL_TRUNCATE_EXTERNAL",
     "legacy external routing rejects an unrepresentable APIC ID"),
    ("LOGIT_X2APIC_NEGCTL_SHORT_DELAY",
     "AP startup obeys the Intel INIT/SIPI wall-time minima"),
    ("LOGIT_X2APIC_NEGCTL_IGNORE_BUSY",
     "stuck xAPIC delivery status aborts the IPI"),
    ("LOGIT_X2APIC_NEGCTL_COMMIT_PUBLISHING",
     "PUBLISHING AP is excluded from the dense online set"),
    ("LOGIT_X2APIC_NEGCTL_FREE_TIMEOUT",
     "a timed-out AP stack is quarantined after startup was sent"),
    ("LOGIT_X2APIC_NEGCTL_AP_TIMER_PIC_EOI",
     "wide-BSP PIC fallback still EOIs an AP LAPIC timer locally"),
]

variants = controls if a.negative_only else [("", "")]
for macro, marker in variants:
    name = macro.lower() if macro else "positive"
    out = base / name
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "x2apic-host"
    cmd = list(common)
    if macro:
        cmd.append("-D" + macro)
    cmd += [str(p) for p in sources] + ["-o", str(exe)]
    subprocess.run(cmd, check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
    (out / "result.log").write_text(result.stdout + result.stderr)
    if macro:
        failed = [line for line in result.stdout.splitlines() if line.startswith("FAIL: ")]
        assert result.returncode != 0, f"{macro} unexpectedly passed"
        assert failed == ["FAIL: " + marker], (macro, failed, result.stderr)
        assert "X2APIC_HOST: 1 checks, 1 failures" in result.stdout, result.stdout
        print(f"{macro}: RED ({marker})")
    else:
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert "X2APIC_HOST: 39 checks, 0 failures" in result.stdout, result.stdout
        print(result.stdout.strip())
