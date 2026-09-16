#!/usr/bin/env python3
"""Comprehension scope, evaluation order, filtering and construction roots."""

import argparse
import json
from pathlib import Path
import re
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT

FIXTURE = ROOT / "tests/fixtures/astyped/comprehension/main.as"


def exercise(compiler):
    cases = {
        "no-leak": ("values = [x for x in range(2)]\n    print(x)", "AS3205"),
        "self-iterable": ("values = [x for x in x]", "AS3205"),
        "bad-filter": ("values = [x for x in range(2) if x]", "AS3202"),
        "bad-iterable": ("values = [x for x in 2]", "AS3202"),
        "bad-context": ("values: List[str] = [x for x in range(2)]", "AS3202"),
        "bad-optional": ("maybe: Optional[i64] = None\n    values = [x for x in range(2) if maybe != None]\n    print(maybe + 1)", "AS3202"),
        "void-element": ("values = [print(x) for x in range(2)]", "AS3202"),
        "array-context": ("values: Array[i64, 2] = [x for x in range(2)]", "AS3202"),
        "later-local": ("values = [later for x in range(2)]\n    later = 3", "AS3206"),
    }
    with tempfile.TemporaryDirectory(prefix="as-comprehension-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\ndef main() -> None:\n    " + body + "\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), (name, report)
        ir = work / "comprehension.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "comprehension", optimization)
            assert result.returncode == 0 and result.stdout == "native comprehensions ok\n", result
    print(f"PASS comprehension: {len(cases)} diagnostics, scoped names, filter order, nested loops, O0/O2 ASan")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-comprehension-control-") as temporary:
        work = Path(temporary)
        ir = work / "comprehension.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "positive", "-O0")
        assert result.returncode == 0, result
        text, count = re.subn(
            r"^  call void @at_gc_root\(ptr %root\d+, ptr %construction\d+, ptr @scan\d+\)\n",
            "", ir.read_text(), flags=re.MULTILINE)
        assert count > 0, "comprehension construction-root mutation anchor changed"
        ir.write_text(text)
        result = sanitized(ir, RUNTIME, work / "negative", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS comprehension control: missing output construction roots fail under ASan")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
