#!/usr/bin/env python3
"""Exercise raw integer pointers through the native compiler, never the VM.

All runtime accesses use owned storage. Negative controls change signedness or
authority checks, not addresses: a failing assertion is enough to prove the
oracle observes the operation without manufacturing invalid memory accesses.
"""

import argparse
import json
from pathlib import Path
import tempfile
from textwrap import dedent

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/pointer/main.as"


def require_output(result, expected="native typed pointers ok\n"):
    assert result.returncode == 0 and result.stdout == expected, result


def diagnostics(compiler, work):
    cases = {
        "constructor-outside": ("def main() -> None:\n    i32ptr(0)", "AS3810"),
        "read-outside": ("def read(p: Ptr[i32]) -> i32:\n    return p[0]", "AS3810"),
        "write-outside": ("def write(p: Ptr[i32]) -> None:\n    p[0] = 3", "AS3810"),
        "compound-outside": ("def add(p: Ptr[i32]) -> None:\n    p[0] += 3", "AS3810"),
        "nested-function": ("""
            def main() -> None:
                unsafe:
                    def read(p: Ptr[i32]) -> i32:
                        return p[0]
            """, "AS3810"),
        "arity": ("def main() -> None:\n    unsafe:\n        i32ptr()", "AS3204"),
        "signed-address": ("def make(a: i64) -> None:\n    unsafe:\n        i32ptr(a)", "AS3202"),
        "managed-element": ("def f(p: Ptr[str]) -> None:\n    pass", "AS3812"),
        "void-element": ("def f(p: Ptr[None]) -> None:\n    pass", "AS3812"),
        "unconstrained-element": ("def f[T](p: Ptr[T]) -> None:\n    pass", "AS3812"),
        "float-element": ("def f(p: Ptr[f64]) -> None:\n    pass", "AS3812"),
        "wrong-width": ("""
            def f(p: Ptr[i8]) -> None:
                unsafe:
                    p[0] = 128
            """, "AS3203"),
        "nonliteral-conversion": ("""
            def f(p: Ptr[i32], value: i64) -> None:
                unsafe:
                    p[0] = value
            """, "AS3202"),
        "wrong-pointer": ("""
            def f() -> None:
                unsafe:
                    p: Ptr[i32] = i16ptr(0)
            """, "AS3202"),
        "unbounded-length": ("def f(p: Ptr[i32]) -> i64:\n    return len(p)", "AS3202"),
    }
    for name, (body, code) in cases.items():
        source = work / (name + ".as")
        source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n") + "\n")
        result = invoke([compiler, "check", source, "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(
            item["code"] == code for item in report["diagnostics"]), (name, report)
    return len(cases)


def exercise(compiler, work):
    count = diagnostics(compiler, work)
    ir = work / "pointer.ll"
    emit_ir(compiler, FIXTURE, ir)
    for optimization in ("-O0", "-O2"):
        require_output(sanitized(ir, RUNTIME, work / "pointer", optimization))
    emit_ir(compiler, FIXTURE.with_name("order.as"), ir)
    for optimization in ("-O0", "-O2"):
        require_output(sanitized(ir, RUNTIME, work / "order", optimization),
                       "native pointer evaluation order ok\n")
    failure = FIXTURE.with_name("failure.as")
    emit_ir(compiler, failure, ir)
    for optimization in ("-O0", "-O2"):
        result = sanitized(ir, RUNTIME, work / "failure", optimization)
        assert result.returncode == 1 and "ValueError" in result.stderr, result
        assert str(failure) + ":5:" in result.stderr, result
    print(f"PASS pointers: {count} diagnostics, widths, alignment, generics, Any, Optional, "
          "negative indices and checked address arithmetic, O0/O2 ASan")


def negative_controls(compiler, work):
    ir = work / "pointer.ll"
    emit_ir(compiler, FIXTURE, ir)
    require_output(sanitized(ir, RUNTIME, work / "positive", "-O0"))
    # Move one actual store by one byte, still within the owner's 64 bytes.
    # The independent byte checks must notice the changed physical layout.
    source = work / "wrong-layout.as"
    text = FIXTURE.read_text()
    anchor = "short = i16ptr(base + u64(1))"
    assert text.count(anchor) == 1
    source.write_text(text.replace(anchor, "short = i16ptr(base + u64(2))"))
    emit_ir(compiler, source, ir)
    result = sanitized(ir, RUNTIME, work / "wrong-layout", "-O0")
    assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS pointer control: shifted typed store changes actual owned bytes")

    runtime = work / "revocation-runtime"
    runtime.mkdir()
    copy_runtime(runtime)
    heap = runtime / "heap.c"
    text = heap.read_text()
    anchor = "void at_gc_collect(void)\n{"
    assert text.count(anchor) == 1
    heap.write_text(text.replace(anchor, anchor + "\n    at_caps_set(0, NULL);"))
    emit_ir(compiler, FIXTURE.with_name("revoked.as"), ir)
    require_output(sanitized(ir, runtime, work / "revoked", "-O0"),
                   "native pointer revocation ok\n")
    capability = runtime / "capability.c"
    text = capability.read_text()
    anchor = "return (held_bits & bits) == bits;"
    assert text.count(anchor) == 1
    capability.write_text(text.replace(anchor, "return 1;"))
    result = sanitized(ir, runtime, work / "unguarded", "-O0")
    assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS pointer control: ignored capability revocation fails on live owned memory")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-native-pointer-") as directory:
        work = Path(directory)
        if args.negative_control:
            negative_controls(args.compiler.resolve(), work)
        else:
            exercise(args.compiler.resolve(), work)


if __name__ == "__main__":
    main()
