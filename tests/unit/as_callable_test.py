#!/usr/bin/env python3
"""First-class native function signatures, generic callbacks and propagation."""

import argparse
import json
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/callable/main.as"


def exercise(compiler):
    cases = {
        "not-callable": ("""
            def main() -> None:
                number = 1
                number()
            """, "AS3202"),
        "wrong-argument": ("""
            def identity(value: i64) -> i64:
                return value
            def main() -> None:
                callback = identity
                callback("text")
            """, "AS3202"),
        "wrong-result": ("""
            def identity(value: i64) -> i64:
                return value
            def main() -> None:
                callback: Callable[[i64], str] = identity
            """, "AS3202"),
        "wrong-arity": ("""
            def identity(value: i64) -> i64:
                return value
            def main() -> None:
                callback = identity
                callback()
            """, "AS3204"),
        "uninitialized": ("""
            def main() -> None:
                callback: Callable[[], None]
                callback()
            """, "AS3206"),
        "missing-context": ("""
            def identity[T](value: T) -> T:
                return value
            def main() -> None:
                callback = identity
            """, "AS3602"),
        "wrong-constraint": ("""
            def square[T: Number](value: T) -> T:
                return value * value
            def main() -> None:
                callback: Callable[[str], str] = square
            """, "AS3601"),
        "void-parameter": ("""
            def consume(callback: Callable[[None], i64]) -> None:
                pass
            """, "AS3202"),
        "incomplete": ("""
            def consume(callback: Callable[[i64],) -> None:
                pass
            """, "AS3100"),
    }
    with tempfile.TemporaryDirectory(prefix="as-callable-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == code for item in report["diagnostics"]), (name, report)
        ir = work / "callable.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "callable", optimization)
            assert result.returncode == 0 and result.stdout == "native callable ok\n", result
    print(f"PASS native Callable: {len(cases)} diagnostics, O0/O2 generic callbacks, storage and exceptions")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-callable-control-") as temporary:
        work = Path(temporary)
        ir = work / "callable.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "callable-ok", "-O0")
        assert result.returncode == 0, result
        # Only indirect calls lose their pending-error check. A failed callback
        # then allows the next side effect, which the same fixture must catch.
        text, changed = re.subn(
            r"(  (?:%v\d+ = )?call [^\n]+ %v\d+\([^\n]+\)\n  %v\d+ = )load i32, ptr @at_failed",
            r"\g<1>add i32 0, 0", ir.read_text())
        assert changed > 0, "indirect propagation mutation anchor changed"
        ir.write_text(text)
        result = sanitized(ir, RUNTIME, work / "callable-broken", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS negative control: missing indirect exception check observed allowing a forbidden side effect")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())


if __name__ == "__main__":
    main()
