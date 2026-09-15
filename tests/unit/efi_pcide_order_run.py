#!/usr/bin/env python3
"""Assemble the real UEFI trampoline and prove the PCIDE/PG order is load-bearing."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "c/boot/efi/trampoline.S"


def tool(name: str, fallback: str | None = None) -> str:
    found = shutil.which(name)
    if found:
        return found
    if fallback and Path(fallback).is_file():
        return fallback
    raise SystemExit(f"missing required tool: {name}")


def assemble(build: Path, name: str, defines: tuple[str, ...]) -> str:
    clang = tool("clang")
    objdump = tool("llvm-objdump", "/opt/homebrew/opt/llvm/bin/llvm-objdump")
    obj = build / f"trampoline-{name}.obj"
    cmd = [clang, "--target=x86_64-unknown-windows", "-c", str(SOURCE),
           "-o", str(obj), *(f"-D{x}" for x in defines)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return subprocess.run([objdump, "-d", str(obj)], cwd=ROOT, check=True,
                          text=True, stdout=subprocess.PIPE).stdout


def ordered_gate(disassembly: str) -> bool:
    """One verdict: clear+write+fresh readback precede far jump and PG clear."""
    clear = disassembly.find("andq\t$-0x20001, %rax")
    write = disassembly.find("movq\t%rax, %cr4", clear + 1)
    read = disassembly.find("movq\t%cr4, %rax", write + 1)
    check = disassembly.find("testq\t$0x20000, %rax", read + 1)
    reject = disassembly.find("\tjne\t", check + 1)
    far = disassembly.find("ljmpl", reject + 1)
    pg_off = disassembly.find("movq\t%rax, %cr0", far + 1)
    return 0 <= clear < write < read < check < reject < far < pg_off


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, type=Path)
    args = ap.parse_args()
    args.build.mkdir(parents=True, exist_ok=True)

    variants = (
        ("normal", (), True),
        ("forced", ("EFI_FORCE_PCIDE",), True),
        ("no-readback", ("EFI_FORCE_PCIDE", "EFI_PCIDE_NEGCTL_SKIP_READBACK"), False),
        ("skip", ("EFI_FORCE_PCIDE", "EFI_PCIDE_NEGCTL_SKIP_CLEAR"), False),
        ("late", ("EFI_FORCE_PCIDE", "EFI_PCIDE_NEGCTL_LATE_CLEAR"), False),
    )
    passed = 0
    for name, defines, expected in variants:
        actual = ordered_gate(assemble(args.build, name, defines))
        if actual != expected:
            print(f"FAIL {name}: ordered PCIDE gate={actual}, expected {expected}",
                  file=sys.stderr)
            return 1
        passed += 1
        if expected:
            print(f"PASS {name}: PCIDE clear/write/readback precedes far jump and CR0.PG clear")
        else:
            print(f"CONTROL RED {name}: ordered production-assembly gate failed exactly 1/1")
    print(f"EFI_PCIDE_ORDER: {passed}/{len(variants)} variants passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
