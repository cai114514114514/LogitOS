#!/usr/bin/env python3
"""Compile the production SMP topology join and require focused mutations."""
from pathlib import Path
import argparse
import os
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--negative-only", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    sources = [
        root / "tests/unit/raptor_smp_test.c",
        root / "c/kernel/cpu/smp_topology.c",
        root / "c/kernel/cpu/apic_model.c",
        root / "c/kernel/cpu/smp_boot_model.c",
    ]
    common = [
        os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-DLOGIT_SMP_BOOT_HOST",
        "-I" + str(root / "c/kernel/cpu"),
    ]
    controls = [
        ("LOGIT_RAPTOR_SMP_NEGCTL_APIC8",
         "wide MADT x2APIC IDs remain distinct in SMP slots"),
        ("LOGIT_RAPTOR_SMP_NEGCTL_ASSUME_SMT2",
         "8 SMT P-cores plus 12 single-thread E-cores report 20 physical cores"),
        ("LOGIT_RAPTOR_SMP_NEGCTL_ALL_P",
         "CPUID.1A core classes survive per-CPU recording and aggregation"),
        ("LOGIT_RAPTOR_SMP_NEGCTL_COMMIT_PUBLISHING",
         "PUBLISHING timeout cannot enter the dense online set"),
    ]
    variants = controls if args.negative_only else [("", "")]
    for macro, marker in variants:
        name = macro.lower() if macro else "positive"
        out = build / name
        out.mkdir(parents=True, exist_ok=True)
        exe = out / "raptor-smp-host"
        cmd = list(common)
        if macro:
            cmd.append("-D" + macro)
        cmd += [str(path) for path in sources] + ["-o", str(exe)]
        subprocess.run(cmd, check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        (out / "result.log").write_text(result.stdout + result.stderr)
        if macro:
            failed = [line for line in result.stdout.splitlines()
                      if line.startswith("FAIL: ")]
            assert result.returncode != 0, f"{macro} unexpectedly passed"
            assert failed == ["FAIL: " + marker], (macro, failed, result.stderr)
            assert "RAPTOR_SMP_HOST: 1 checks, 1 failures" in result.stdout
            print(f"{macro}: RED ({marker})")
        else:
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert "RAPTOR_SMP_HOST: 15 checks, 0 failures" in result.stdout
            print(result.stdout.strip())


if __name__ == "__main__":
    main()
