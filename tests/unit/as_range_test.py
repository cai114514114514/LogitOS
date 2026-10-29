#!/usr/bin/env python3
"""Lazy range values, full-width arithmetic and byte-oriented string iteration."""

import argparse
import json
from pathlib import Path
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/range/main.as"


def exercise(compiler):
    cases = {
        "immutable": ("values = range(2)\n    values[0] = 3", "AS3400"),
        "float-bound": ("values = range(2.5)", "AS3202"),
        "float-key": ("found = 1.0 in range(3)", "AS3202"),
        "bad-index": ('value = range(3)["x"]', "AS3202"),
        "arity": ("values = range()", "AS3204"),
        "range-order": ("value = range(3) < range(4)", "AS3202"),
    }
    with tempfile.TemporaryDirectory(prefix="as-range-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            entry = work / (name + ".as")
            entry.write_text("# aether: 3.0\ndef main() -> None:\n    " + body + "\n")
            result = invoke([compiler, "check", entry, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), (name, report)
        ir = work / "range.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "range", optimization)
            assert result.returncode == 0 and result.stdout == "native ranges and text iteration ok\n", result
    print(f"PASS range: {len(cases)} diagnostics, lazy values, signed limits, text iteration, O0/O2 ASan")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-range-control-") as temporary:
        work = Path(temporary)
        ir = work / "range.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "positive", "-O0")
        assert result.returncode == 0, result
        copy_runtime(work)
        source = work / "range.c"
        text = source.read_text()
        anchor = "span / stride + (span % stride != 0)"
        assert text.count(anchor) == 1, "range count mutation anchor changed"
        source.write_text(text.replace(anchor, "(span + stride - 1) / stride"))
        result = sanitized(ir, work, work / "negative", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS range control: overflowing rounded count observed failing")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
