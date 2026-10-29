#!/usr/bin/env python3
"""Native byte storage, checked writes and managed buffer lifetime."""

import argparse
import json
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/buffer/main.as"
EXPECTED = "native buffers ok\n"


def require_output(result):
    assert result.returncode == 0 and result.stdout == EXPECTED, result


def exercise(compiler):
    cases = {
        "arity": ("def main() -> None:\n    buffer()\n", "AS3204"),
        "float-size": ("def main() -> None:\n    buffer(1.5)\n", "AS3202"),
        "float-byte": ("""
            def main() -> None:
                data = buffer(2)
                data[0] = 1.5
            """, "AS3202"),
        "text-index": ("""
            def read(data: Buffer) -> i64:
                return data["0"]
            """, "AS3202"),
        "wrong-result": ("""
            def read(data: Buffer) -> str:
                return data[0]
            """, "AS3202"),
        "ordered": ("""
            def less(left: Buffer, right: Buffer) -> bool:
                return left < right
            """, "AS3202"),
    }
    with tempfile.TemporaryDirectory(prefix="as-buffer-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == code for item in report["diagnostics"]), (name, report)
        ir = work / "buffer.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(sanitized(ir, RUNTIME, work / "buffer", optimization))
    print(f"PASS native buffers: {len(cases)} diagnostics, byte writes, iteration, Any and GC, O0/O2")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-buffer-control-") as temporary:
        work = Path(temporary)
        ir = work / "buffer.ll"
        emit_ir(compiler, FIXTURE, ir)
        source = ir.read_text()
        copy_runtime(work)
        heap = work / "heap.c"
        original = heap.read_text()
        anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
        assert original.count(anchor) == 1, "allocation mutation anchor changed"
        heap.write_text(original.replace(anchor, anchor + "\n    at_gc_collect();"))
        require_output(sanitized(ir, work, work / "positive", "-O0"))

        # An ordinary i64 store is the tempting implementation error: the
        # source value is i64, but the destination is exactly one byte wide.
        pattern = (r"  (%v\d+) = trunc i64 (%v\d+|\d+) to i8\n"
                   r"  store i8 \1, ptr (%v\d+)\n")
        def wide_store(match):
            return f"  store i64 {match[2]}, ptr {match[3]}\n"
        mutated, count = re.subn(pattern, wide_store, source)
        assert count > 0, "byte store mutation anchor changed"
        ir.write_text(mutated)
        result = sanitized(ir, work, work / "wide-store", "-O0")
        assert result.returncode != 0 and "heap-buffer-overflow" in result.stderr, result

        # Keeping truncation but accepting 256 would silently write zero.
        mutated, count = re.subn(r"(icmp ugt i64 %v\d+, )255\n", r"\g<1>65535\n", source)
        assert count > 0, "byte value mutation anchor changed"
        ir.write_text(mutated)
        result = sanitized(ir, work, work / "truncated-value", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result

        # Isolate root loss to Buffer: no other managed type participates in
        # this program, so an unrelated object's root cannot satisfy the test.
        program = work / "roots.as"
        program.write_text("""# aether: 3.0
def make() -> Buffer:
    data = buffer(1)
    data[0] = 42
    return data
def main() -> None:
    data = make()
    gc_collect()
    assert data[0] == 42
""")
        emit_ir(compiler, program, ir)
        result = sanitized(ir, work, work / "root-positive", "-O0")
        assert result.returncode == 0, result
        mutated, count = re.subn(r"^  call void @at_gc_root\([^\n]+\n", "", ir.read_text(),
                                 flags=re.MULTILINE)
        assert count > 0, "buffer root mutation anchor changed"
        ir.write_text(mutated)
        result = sanitized(ir, work, work / "root-negative", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS buffer controls: wide stores and missing roots fail under ASan; byte truncation fails")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
