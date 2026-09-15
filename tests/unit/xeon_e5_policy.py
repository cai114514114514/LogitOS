#!/usr/bin/env python3
from pathlib import Path
import argparse
import re
import sys

ROOT = Path(__file__).resolve().parents[2]


def osxsave_errors(text, name):
    errors = []
    if "and rax, ~(1 << 18)" not in text:
        errors.append(f"{name}: CR4.OSXSAVE is not explicitly cleared")
    return errors


def lockdiag_errors(text):
    errors = []
    if "lock_put_uint((unsigned int)me)" not in text:
        errors.append("spinlock.c: releasing CPU index is not printed in full")
    if "lock_put_uint((unsigned int)owner)" not in text:
        errors.append("spinlock.c: owning CPU index is not printed in full")
    if re.search(r"(?:me|owner)\s*&\s*7", text):
        errors.append("spinlock.c: high CPU index is truncated to three bits")
    return errors


def tree_errors(root):
    errors = []
    for rel in ("c/boot/long.asm", "c/boot/ap_trampoline.asm"):
        errors += osxsave_errors((root / rel).read_text(), rel)
    errors += lockdiag_errors((root / "c/kernel/cpu/spinlock.c").read_text())

    percpu = (root / "c/kernel/cpu/smp/percpu.h").read_text()
    for token in ("#define PERCPU_MAXCPU 32", "#define PERCPU_MAXCPU 8",
                  "LOGIT_CPU_CAP_NEGCTL"):
        if token not in percpu:
            errors.append(f"percpu.h: missing {token}")

    constants = {
        "c/kernel/mm/phys/kheap.c": r"#define\s+MAG_MAXCPU\s+32\b",
        "c/kernel/sched/kbench.h": r"#define\s+KB_MAXCPU\s+32\b",
        "c/kernel/diag/kprof.h": r"#define\s+KPROF_MAXCPU\s+32\b",
    }
    for rel, pattern in constants.items():
        if not re.search(pattern, (root / rel).read_text()):
            errors.append(f"{rel}: per-CPU capacity is not 32")

    matches = []
    for path in sorted((root / "include/abi").glob("*.h")):
        matches += re.findall(
            r"^#define\s+(SYS_[A-Z0-9_]+)\s+(\d+)\b",
            path.read_text(), re.M)
    owners = {}
    for name, number in matches:
        owners.setdefault(int(number), []).append(name)
    if owners.get(196) != ["SYS_CPU_COUNT"]:
        errors.append(f"ABI 196 owner is {owners.get(196)}")
    clashes = {n: names for n, names in owners.items() if len(names) > 1}
    if clashes:
        errors.append(f"duplicate SYS_* numbers: {clashes}")

    syscall = (root / "c/kernel/exec/syscall.c").read_text()
    if not re.search(r"case\s+SYS_CPU_COUNT\s*:.*?smp_cpu_count\s*\(",
                     syscall, re.S):
        errors.append("SYS_CPU_COUNT is not backed by smp_cpu_count()")
    libc = (root / "c/apps/libc/src/io.c").read_text()
    online = re.search(
        r"case\s+_SC_NPROCESSORS_ONLN\s*:(.*?)(?:case|default)",
        libc, re.S)
    if not online or "SYS_CPU_COUNT" not in online.group(1):
        errors.append("_SC_NPROCESSORS_ONLN does not query SYS_CPU_COUNT")
    agent_policy = (root / "include/abi/agent_policy.h").read_text()
    if not re.search(r"case\s+SYS_CPU_COUNT\s*:", agent_policy):
        errors.append("agent worker policy denies read-only SYS_CPU_COUNT")
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--negative-osxsave", action="store_true")
    parser.add_argument("--negative-cpucount", action="store_true")
    parser.add_argument("--negative-lockdiag", action="store_true")
    parser.add_argument("--write-platform-negctl", type=Path)
    args = parser.parse_args()

    errors = tree_errors(ROOT)
    if errors:
        print("\n".join(f"FAIL: {error}" for error in errors))
        return 1
    if args.negative_osxsave:
        text = (ROOT / "c/boot/long.asm").read_text()
        mutated = text.replace(
            "and rax, ~(1 << 18)", "or  rax, (1 << 18)", 1)
        caught = osxsave_errors(mutated, "inherited-loader fixture")
        if not caught:
            print("NEGCTL FAIL: inherited CR4.OSXSAVE passed")
            return 1
        print("NEGCTL RED:", caught[0])
    if args.negative_cpucount:
        libc = (ROOT / "c/apps/libc/src/io.c").read_text()
        mutated = libc.replace("SYS_CPU_COUNT", "SYS_CPU_INDEX", 1)
        block = re.search(
            r"case\s+_SC_NPROCESSORS_ONLN\s*:(.*?)(?:case|default)",
            mutated, re.S)
        if block and "SYS_CPU_COUNT" in block.group(1):
            print("NEGCTL FAIL: CPU-index sysconf passed")
            return 1
        print("NEGCTL RED: CPU-index sysconf regression rejected")
    if args.negative_lockdiag:
        text = (ROOT / "c/kernel/cpu/spinlock.c").read_text()
        mutated = text.replace(
            "lock_put_uint((unsigned int)me)",
            "serial_putc((char)('0' + (me & 7)))", 1)
        caught = lockdiag_errors(mutated)
        if not caught:
            print("NEGCTL FAIL: truncated high-CPU lock diagnostic passed")
            return 1
        print("NEGCTL RED:", caught[0])
    if args.write_platform_negctl:
        source = (ROOT / "c/kernel/cpu/cpu_platform.c").read_text()
        needle = "top->leaf_b = 1;"
        if source.count(needle) != 1:
            print("NEGCTL FAIL: CPUID.0B mutation seam is not unique")
            return 1
        args.write_platform_negctl.write_text(
            source.replace(needle, "top->leaf_b = 0;", 1))
        print("wrote CPUID.0B negative-control source")
    print("xeon e5 policy: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
