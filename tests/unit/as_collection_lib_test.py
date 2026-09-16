#!/usr/bin/env python3
"""Rewritten seq/dicts APIs, callback ordering, pair conversions and equality."""

import argparse
import json
from pathlib import Path
import shutil
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, LIBRARY

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = {
    "seq": "native sequence library ok\n",
    "dicts": "native dictionary library ok\n",
}


def exercise(compiler):
    cases = {
        "unconstrained-comparison": ("""
            def equal[T](a: T, b: T) -> bool:
                return a == b
            """, "AS3202"),
        "unordered-list": ("""
            def main() -> None:
                assert [1] < [2]
            """, "AS3202"),
        "unconstrained-membership": ("""
            def contains[T](value: T, values: List[T]) -> bool:
                return value in values
            """, "AS3601"),
        "void-argument": ("""
            def identity[T](value: T) -> T:
                return value
            def main() -> None:
                identity(print(1))
            """, "AS3202"),
        "untyped-pair-result": ("""
            import dicts
            def main() -> None:
                result = dicts.from_pairs([[Any("key"), Any(1)]])
            """, "AS3602"),
    }
    with tempfile.TemporaryDirectory(prefix="as-collections-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--stdlib", LIBRARY, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == code for item in report["diagnostics"]), (name, report)
        for name, output in FIXTURES.items():
            ir = work / (name + ".ll")
            emit_ir(compiler, ROOT / "tests/fixtures/astyped" / name / "main.as", ir)
            for optimization in ("-O0", "-O2"):
                result = sanitized(ir, RUNTIME, work / name, optimization)
                assert result.returncode == 0 and result.stdout == output, result
    print("PASS native seq/dicts: all public functions, O0/O2 sanitizer checks, callbacks, equality and pairs")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-collections-control-") as temporary:
        work = Path(temporary)
        fixture = ROOT / "tests/fixtures/astyped/seq/main.as"
        ir = work / "seq.ll"
        emit_ir(compiler, fixture, ir)
        result = sanitized(ir, RUNTIME, work / "seq-ok", "-O0")
        assert result.returncode == 0, result
        shutil.copytree(LIBRARY, work / "lib")
        source = work / "lib/seq.as"
        text = source.read_text()
        anchor = "less(key, out[previous])"
        assert text.count(anchor) == 1, "sort comparator mutation anchor changed"
        source.write_text(text.replace(anchor, "less(out[previous], key)"))
        result = invoke([compiler, "build", fixture, "--stdlib", work / "lib", "--emit-llvm", "-o", ir])
        assert result.returncode == 0, result
        result = sanitized(ir, RUNTIME, work / "seq-broken", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS negative control: reversed native comparator observed failing the collection oracle")


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
