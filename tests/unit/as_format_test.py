#!/usr/bin/env python3
"""Native layout descriptions, recursive formatting and output ordering."""

import argparse
from pathlib import Path
import shutil
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, LIBRARY
from as_runtime import copy_runtime

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/format/main.as"
EXPECTED = "argument first\n[1] 2\ndynamic {'answer': 42} [true, false]\nnative formatting ok\n"
TEST_LIBRARY = ROOT / "tests/fixtures/astyped/test-lib/main.as"
TEST_OUTPUT = (
    "FAIL: recorded\nFAIL: list identity -- got [1] want [1]\n"
    "FAIL: same text -- both x\nFAIL: different values\nFAIL: explicit\n"
    "tests: 7 passed, 5 failed\ntests: 0 passed, 0 failed\nnative test library ok\n"
)


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-format-") as temporary:
        work = Path(temporary)
        ir = work / "format.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "format", optimization)
            assert result.returncode == 0 and result.stdout == EXPECTED, result
        test_ir = work / "test-lib.ll"
        emit_ir(compiler, TEST_LIBRARY, test_ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(test_ir, RUNTIME, work / "test-lib", optimization)
            assert result.returncode == 0 and result.stdout == TEST_OUTPUT, result
    print("PASS native formatting: O0/O2 mixed layouts, Any, cycles, long text, GC and argument order")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-format-control-") as temporary:
        work = Path(temporary)
        ir = work / "format.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "format-ok", "-O0")
        assert result.returncode == 0, result
        copy_runtime(work)
        source = work / "format.c"
        text = source.read_text()
        anchor = "(const unsigned char *)value + field->offset"
        assert text.count(anchor) == 1, "native field layout mutation anchor changed"
        # Re-read the first bool byte for the u8 field only. Other fields keep
        # valid pointers and lengths, so the failure must be a wrong-value
        # assertion rather than a crash while interpreting invalid metadata.
        replacement = (
            "(const unsigned char *)value + "
            "(field->type->kind == AT_NATIVE_INTEGER && field->type->bits == 8 ? 0 : field->offset)"
        )
        source.write_text(text.replace(anchor, replacement))
        result = sanitized(ir, work, work / "format-broken", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        shutil.copytree(LIBRARY, work / "lib")
        source = work / "lib/test.as"
        text = source.read_text()
        assert text.count("_failed += 1") == 1, "test counter mutation anchor changed"
        source.write_text(text.replace("_failed += 1", "_failed += 0"))
        result = invoke([compiler, "build", TEST_LIBRARY, "--stdlib", work / "lib", "--emit-llvm", "-o", ir])
        assert result.returncode == 0, result
        result = sanitized(ir, RUNTIME, work / "test-lib-broken", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS negative control: discarded native field offsets observed failing layout formatting")
    print("PASS negative control: disabled test failure counter observed failing the report oracle")


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
