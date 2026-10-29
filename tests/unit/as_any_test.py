#!/usr/bin/env python3
"""Explicit dynamic boxes retain native payloads, type checks and GC ownership."""

import argparse
import json
from pathlib import Path
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME
from as_runtime import copy_runtime

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/any/main.as"


def exercise(compiler):
    cases = {
        "implicit-box": (
            """
            def main() -> None:
                value: Any = 1
            """,
            "AS3202",
        ),
        "cast-static": (
            """
            def main() -> None:
                cast[i64](1)
            """,
            "AS3202",
        ),
        "cast-void": (
            """
            def main() -> None:
                cast[None](Any(1))
            """,
            "AS3202",
        ),
        "box-void": (
            """
            def nothing() -> None:
                pass
            def main() -> None:
                Any(nothing())
            """,
            "AS3202",
        ),
        "borrow": (
            """
            def box(view: Slice[i64]) -> Any:
                return Any(view)
            """,
            "AS3400",
        ),
        "nested-borrow": (
            """
            struct View:
                data: Slice[i64]
            def box(view: Slice[i64]) -> Any:
                return Any(View(view))
            """,
            "AS3400",
        ),
        "empty-box": (
            """
            def main() -> None:
                Any()
            """,
            "AS3204",
        ),
        "empty-cast": (
            """
            def main() -> None:
                cast[i64]()
            """,
            "AS3204",
        ),
        "type-value": (
            """
            def main() -> None:
                cast[i64]
            """,
            "AS3900",
        ),
    }
    with tempfile.TemporaryDirectory(prefix="as-any-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                diagnostic["code"] == code for diagnostic in report["diagnostics"]), (name, report)
        ir = work / "any.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "any", optimization)
            assert result.returncode == 0 and result.stdout == "explicit Any ok\n", result
    print(f"PASS explicit Any: {len(cases)} diagnostics, O0/O2 payloads, casts, generics and roots")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-any-control-") as temporary:
        work = Path(temporary)
        ir = work / "any.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "any-ok", "-O0")
        assert result.returncode == 0, result
        copy_runtime(work)
        source = work / "any.c"
        text = source.read_text()
        anchor = "box->scan(box->data);"
        assert text.count(anchor) == 1, "Any payload scanner mutation anchor changed"
        source.write_text(text.replace(anchor, "return;"))
        result = sanitized(ir, work, work / "any-broken", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
        copy_runtime(work)
        source = work / "equal.c"
        text = source.read_text()
        anchor = "return a == b;"
        assert text.count(anchor) == 2, "floating equality mutation anchor changed"
        source.write_text(text.replace(anchor, "return !memcmp(left, right, sizeof a);"))
        result = sanitized(ir, work, work / "any-equality-broken", "-O0")
        assert result.returncode != 0 and "AssertionError" in result.stderr, result
    print("PASS negative controls: unscanned Any payload and bitwise float equality fail")


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
