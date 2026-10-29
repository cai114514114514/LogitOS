#!/usr/bin/env python3
"""Rewritten native statistics, exact integer medians and promotion order."""

import argparse
from pathlib import Path
import shutil
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, LIBRARY

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/stats/main.as"


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-stats-") as temporary:
        work = Path(temporary)
        ir = work / "stats.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "stats", optimization)
            assert result.returncode == 0 and result.stdout == "native stats library ok\n", result
    print("PASS native stats: every public function, i64 boundaries, mutation, errors and GC at O0/O2")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-stats-control-") as temporary:
        work = Path(temporary)
        ir = work / "stats.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "stats-ok", "-O0")
        assert result.returncode == 0, result
        shutil.copytree(LIBRARY, work / "lib")
        source = work / "lib/stats.as"
        text = source.read_text()
        anchor = "Any((f64(values[middle - 1]) + f64(values[middle])) / 2.0)"
        assert text.count(anchor) == 1, "median promotion mutation anchor changed"
        source.write_text(text.replace(anchor, "Any(f64(values[middle - 1] + values[middle]) / 2.0)"))
        result = invoke([compiler, "build", FIXTURE, "--stdlib", work / "lib", "--emit-llvm", "-o", ir])
        assert result.returncode == 0, result
        result = sanitized(ir, RUNTIME, work / "stats-broken", "-O0")
        assert result.returncode == 1 and "OverflowError" in result.stderr, result
        assert "native stats library ok" not in result.stdout, result
    print("PASS negative control: integer-before-float median observed overflowing on valid inputs")


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
