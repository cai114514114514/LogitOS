#!/usr/bin/env python3
"""Interpolation uses ordinary static checking and original snapshot locations."""

import argparse
import json
from pathlib import Path
import re
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT

FIXTURE = ROOT / "tests/fixtures/astyped/fstring/main.as"


def exercise(compiler):
    cases = {
        "empty": ('f"{}"', "AS3100"),
        "whitespace": ('f"{   }"', "AS3100"),
        "format-spec": ('f"{3:02}"', "AS3100"),
        "tail": ('f"{1 2}"', "AS3100"),
        "closing": ('f"{1) + 2}"', "AS3100"),
        "unknown": ('f"中文 {missing}"', "AS3205"),
        "mismatch": ('f"中文 {1 + 2.5}"', "AS3202"),
        "void": ('f"{print(1)}"', "AS3202"),
        "lexical": ('f"{!}"', "AS3100"),
        "pieces": ('f"' + '{1}x' * 40 + '"', "AS3100"),
    }
    with tempfile.TemporaryDirectory(prefix="as-fstring-") as temporary:
        work = Path(temporary)
        for name, (expression, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\ndef main() -> None:\n    value = " + expression + "\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), (name, report)
            if name == "unknown":
                error = next(error for error in report["diagnostics"] if error["code"] == code)
                offset = source.read_bytes().index(b"missing")
                assert error["start"] == offset, error
        ir = work / "fstring.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "fstring", optimization)
            assert result.returncode == 0 and result.stdout == "native fstrings ok\n", result
    print(f"PASS native fstrings: {len(cases)} diagnostics, original positions, O0/O2 ASan")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-fstring-control-") as temporary:
        work = Path(temporary)
        ir = work / "fstring.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "positive", "-O0")
        assert result.returncode == 0, result
        text, count = re.subn(
            r"^  call void @at_gc_root\(ptr %root\d+, ptr %rootvalue\d+, ptr @scan\d+\)\n",
            "", ir.read_text(), flags=re.MULTILINE)
        assert count > 0, "f-string expression-root mutation anchor changed"
        ir.write_text(text)
        result = sanitized(ir, RUNTIME, work / "negative", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS native fstring control: missing intermediate roots fail under ASan")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
