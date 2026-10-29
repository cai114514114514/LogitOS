#!/usr/bin/env python3
"""Native scalar conversion behavior and observed parser failure controls."""

import argparse
import json
from pathlib import Path
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME
from as_runtime import copy_runtime

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/conversions/main.as"


def exercise(compiler):
    cases = {
        "parse-type": 'parse_int(7)',
        "float-type": 'parse_float(7)',
        "character-type": 'chr("x")',
        "ordinal-type": 'ord(1)',
        "bits-type": 'f64bits("1")',
        "shadowed-value": 'str = 7\nstr(1)',
    }
    with tempfile.TemporaryDirectory(prefix="as-conversions-") as temporary:
        work = Path(temporary)
        for name, statements in cases.items():
            source = work / (name + ".as")
            body = "\n".join("    " + line for line in statements.splitlines())
            source.write_text("# aether: 3.0\ndef main() -> None:\n" + body + "\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == "AS3202" for item in report["diagnostics"]), (name, report)

        ir = work / "conversions.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "conversions", optimization)
            assert result.returncode == 0 and result.stdout == "scalar conversions ok\n", result

        source = work / "shadowed-function.as"
        source.write_text(dedent("""\
            # aether: 3.0
            def str(value: i64) -> i64:
                return value + 1
            def main() -> None:
                assert str(7) == 8
            """))
        result = invoke([compiler, "run", source, "--toolchain", RUNTIME])
        assert result.returncode == 0, result
    print(f"PASS native conversions: {len(cases)} diagnostics, O0/O2 boundaries, GC and declaration precedence")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-conversion-control-") as temporary:
        work = Path(temporary)
        ir = work / "conversions.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "conversions-ok", "-O0")
        assert result.returncode == 0, result
        copy_runtime(work)
        source = work / "convert.c"
        text = source.read_text()
        anchor = "int valid = end != buffer && end == buffer + length;"
        assert text.count(anchor) == 1, "float-consumption mutation anchor changed"
        source.write_text(text.replace(anchor, "int valid = 1;"))
        result = sanitized(ir, work, work / "conversions-broken", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        assert "scalar conversions ok" not in result.stdout, result
    print("PASS negative control: unchecked float input observed failing the conversion oracle")


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
