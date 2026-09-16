#!/usr/bin/env python3
"""Execute native unsafe loads/stores and verify lexical and authority boundaries."""

import argparse
import json
from pathlib import Path
import tempfile
from textwrap import dedent

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/memory/main.as"
REVOKED = FIXTURE.with_name("revoked.as")
EXPECTED = "native raw memory ok\n"


def require_output(result, expected=EXPECTED):
    assert result.returncode == 0 and result.stdout == expected, result


def exercise(compiler):
    cases = {
        "outside": ("def main() -> None:\n    addr(buffer(1))\n", "AS3810"),
        "after": ("""
            def main() -> None:
                unsafe:
                    address = addr(buffer(1))
                peek8(address)
            """, "AS3810"),
        "nested-function": ("""
            def main() -> None:
                unsafe:
                    def read() -> i64:
                        return peek8(0)
                    read()
            """, "AS3810"),
        "lambda": ("""
            def main() -> None:
                unsafe:
                    read: Callable[[], i64] = lambda: peek8(0)
                    read()
            """, "AS3810"),
        "signed-address": ("""
            def read(address: i64) -> i64:
                unsafe:
                    return peek8(address)
            """, "AS3202"),
        "float-value": ("""
            def main() -> None:
                unsafe:
                    poke8(0, 1.0)
            """, "AS3202"),
        "opaque-owner": ("""
            def main() -> None:
                unsafe:
                    addr(caps())
            """, "AS3202"),
        "peek-arity": ("def main() -> None:\n    unsafe:\n        peek8()\n", "AS3204"),
        "poke-arity": ("def main() -> None:\n    unsafe:\n        poke8(0)\n", "AS3204"),
    }
    with tempfile.TemporaryDirectory(prefix="as-memory-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == code for item in report["diagnostics"]), (name, report)
        ir = work / "memory.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(sanitized(ir, RUNTIME, work / "memory", optimization))
    print(f"PASS native raw memory: {len(cases)} diagnostics, O0/O2, "
          "unaligned access, bit widths, control flow and lexical unsafe")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-memory-control-") as temporary:
        work = Path(temporary)
        copy_runtime(work)
        heap = work / "heap.c"
        original_heap = heap.read_text()
        allocation = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
        assert original_heap.count(allocation) == 1
        heap.write_text(original_heap.replace(allocation, allocation + "\n    at_gc_collect();"))
        ir = work / "memory.ll"
        emit_ir(compiler, FIXTURE, ir)
        require_output(sanitized(ir, work, work / "positive", "-O0"))

        # A 32-bit peek is unsigned; treating its high bit as a sign changes the
        # value of a real read even though its address and width are correct.
        text = ir.read_text()
        assert " = zext i32 " in text
        ir.write_text(text.replace(" = zext i32 ", " = sext i32 "))
        result = sanitized(ir, work, work / "wrong-sign", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result

        # Revocation occurs after this source obtained a valid owned address.
        # Every subsequent raw operation must consult the current held set.
        collect = "void at_gc_collect(void)\n{"
        assert original_heap.count(collect) == 1
        heap.write_text(original_heap.replace(collect, collect + "\n    at_caps_set(0, NULL);"))
        emit_ir(compiler, REVOKED, ir)
        require_output(sanitized(ir, work, work / "revoked", "-O0"),
                       "native raw revocation ok\n")

        capability = work / "capability.c"
        original = capability.read_text()
        anchor = "return (held_bits & bits) == bits;"
        assert original.count(anchor) == 1
        capability.write_text(original.replace(anchor, "return 1;"))
        result = sanitized(ir, work, work / "unguarded", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS raw memory controls: wrong signed load and ignored revocation fail assertions")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
