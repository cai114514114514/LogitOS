#!/usr/bin/env python3
"""Assemble every paging entry path and verify its four-level handoff."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "c/boot/efi/trampoline.S"
BOOT_SOURCE = ROOT / "c/boot/boot.asm"
AP_SOURCE = ROOT / "c/boot/ap_trampoline.asm"
NASM_LA57_CLEAR = "and eax, ~(1 << 12)"


def tool(name: str, fallback: str | None = None) -> str:
    found = shutil.which(name)
    if found:
        return found
    if fallback and Path(fallback).is_file():
        return fallback
    raise SystemExit(f"missing required tool: {name}")


def assemble(build: Path, name: str, defines: tuple[str, ...]) -> str:
    build.mkdir(parents=True, exist_ok=True)
    obj = build / f"trampoline-{name}.obj"
    subprocess.run([
        tool("clang"), "--target=x86_64-unknown-windows", "-c", str(SOURCE),
        "-o", str(obj), *(f"-D{define}" for define in defines),
    ], cwd=ROOT, check=True)
    return subprocess.run([
        tool("llvm-objdump", "/opt/homebrew/opt/llvm/bin/llvm-objdump"),
        "-d", str(obj),
    ], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE).stdout


def assemble_nasm(build: Path, name: str, source: Path) -> str:
    """Assemble one real or mutated NASM entry path and return its disassembly."""
    build.mkdir(parents=True, exist_ok=True)
    obj = build / f"{name}.o"
    subprocess.run([
        tool("nasm"), "-f", "elf64", str(source), "-o", str(obj),
    ], cwd=ROOT, check=True)
    return subprocess.run([
        tool("llvm-objdump", "/opt/homebrew/opt/llvm/bin/llvm-objdump"),
        "-d", str(obj),
    ], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE).stdout


def symbol(disassembly: str, start: str, end: str) -> str:
    begin = disassembly.find(f"<{start}>:")
    finish = disassembly.find(f"<{end}>:", begin + 1)
    if begin < 0 or finish < 0:
        return ""
    return disassembly[begin:finish]


def paging_enable_gate(disassembly: str, start: str, end: str) -> bool:
    """Require LA57=0 in CR4 before this path enables four-level paging."""
    body = symbol(disassembly, start, end)
    instructions = (
        "movq\t%cr4, %rax",
        "andl\t$0xffffefff, %eax",
        "orl\t$0x20, %eax",
        "movq\t%rax, %cr4",
        "orl\t$0x80000000, %eax",
        "movq\t%rax, %cr0",
    )
    points = tuple(body.find(instruction) for instruction in instructions)
    return (
        all(body.count(instruction) == 1 for instruction in instructions)
        and all(point >= 0 for point in points)
        and list(points) == sorted(points)
    )


def delete_nasm_la57_clear(source: Path, destination: Path) -> None:
    """Create the one-instruction mutation used to prove the gate goes red."""
    lines = source.read_text().splitlines(keepends=True)
    matches = [
        index for index, line in enumerate(lines)
        if line.split(";", 1)[0].strip() == NASM_LA57_CLEAR
    ]
    assert len(matches) == 1, (
        f"{source}: expected exactly one canonical LA57 clear, found {len(matches)}"
    )
    del lines[matches[0]]
    destination.write_text("".join(lines))


def ordered_gate(disassembly: str) -> bool:
    """Require LA57 clear/readback after PG/LME off and before kernel entry."""
    far = disassembly.find("ljmpl")
    pg_clear = disassembly.find("andl\t$0x7fffffff, %eax", far + 1)
    pg_write = disassembly.find("movq\t%rax, %cr0", pg_clear + 1)
    lme_clear = disassembly.find("andl\t$0xfffffeff, %eax", pg_write + 1)
    lme_write = disassembly.find("wrmsr", lme_clear + 1)
    la57_clear = disassembly.find("andl\t$0xffffef5f, %eax", lme_write + 1)
    cr4_write = disassembly.find("movq\t%rax, %cr4", la57_clear + 1)
    readback = disassembly.find("movq\t%cr4, %rax", cr4_write + 1)
    check = disassembly.find("testl\t$0x1000, %eax", readback + 1)
    reject = disassembly.find("\tjne\t", check + 1)
    kernel_jump = disassembly.find("jmpq\t*%rdi", reject + 1)
    points = (far, pg_clear, pg_write, lme_clear, lme_write, la57_clear,
              cr4_write, readback, check, reject, kernel_jump)
    return all(point >= 0 for point in points) and list(points) == sorted(points)


def positive(build: Path) -> None:
    normal = assemble(build, "normal", ())
    forced = assemble(build, "forced", (
        "EFI_FORCE_LA57_AFTER_PG", "EFI_LA57_TEST_ASSERT_CLEAR",
    ))
    assert ordered_gate(normal), "ordinary trampoline lost its ordered LA57 gate"
    assert ordered_gate(forced), "forced-LA57 trampoline lost its ordered gate"

    force = forced.find("orl\t$0x1000, %eax")
    production_clear = forced.find("andl\t$0xffffef5f, %eax", force + 1)
    assert 0 <= force < production_clear, "test seam no longer feeds production clear"
    assert "orl\t$0x1000, %eax" not in normal

    boot = assemble_nasm(build, "boot-normal", BOOT_SOURCE)
    ap = assemble_nasm(build, "ap-trampoline-normal", AP_SOURCE)
    assert paging_enable_gate(boot, "enable_paging", "error"), (
        "BIOS/Multiboot BSP path lost its LA57 clear before PAE/PG"
    )
    assert paging_enable_gate(ap, "pm32", "lm64"), (
        "AP startup path lost its LA57 clear before PAE/PG"
    )
    print("PASS: 4 positive objects gated (2 EFI, BIOS BSP, AP startup)")


def controls(build: Path) -> None:
    variants = {
        "skip-clear": (
            "EFI_FORCE_LA57_AFTER_PG", "EFI_LA57_TEST_ASSERT_CLEAR",
            "EFI_LA57_NEGCTL_SKIP_CLEAR",
        ),
        "skip-readback": ("EFI_LA57_NEGCTL_SKIP_READBACK",),
        "early-clear": ("EFI_LA57_NEGCTL_EARLY_CLEAR",),
    }
    for name, defines in variants.items():
        disassembly = assemble(build, name, defines)
        assert not ordered_gate(disassembly), f"CONTROL FAILED: {name} passed"
        print(f"CONTROL RED: EFI {name}")

    build.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="la57-mutant-", dir=build) as tmp:
        temporary = Path(tmp)
        nasm_variants = (
            ("boot-delete-clear", BOOT_SOURCE, "enable_paging", "error"),
            ("ap-delete-clear", AP_SOURCE, "pm32", "lm64"),
        )
        for name, source, start, end in nasm_variants:
            mutant = temporary / source.name
            delete_nasm_la57_clear(source, mutant)
            disassembly = assemble_nasm(build, name, mutant)
            assert not paging_enable_gate(disassembly, start, end), (
                f"CONTROL FAILED: {name} passed after deleting its LA57 clear"
            )
            print(f"CONTROL RED: NASM {name}")
    print("PASS: 5 negative controls red (3 EFI, 2 exact NASM deletions)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--mode", choices=("positive", "controls"), required=True)
    args = parser.parse_args()
    (positive if args.mode == "positive" else controls)(args.build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
