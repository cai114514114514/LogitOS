#!/usr/bin/env python3
"""Mixed numeric expressions preserve storage types and native conversion signs.

The host runs actual emitted LLVM at O0/O2 under ASan/UBSan. The same fixture is
usable by the guest gate; host execution alone does not prove Studio's path.
"""

import argparse
import json
from pathlib import Path
import re
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT

FIXTURE = ROOT / "tests/fixtures/astyped/mixed_numeric/main.as"
SCREENSHOT = "# aether: 3.0\ny = 2\nx = 1.0\nprint(x + y)\n"


def require_output(ir, binary, optimization, expected):
    result = sanitized(ir, RUNTIME, binary, optimization)
    assert result.returncode == 0 and result.stdout == expected, (
        optimization, result.returncode, result.stdout, result.stderr)


def refusal_cases():
    # Promotion belongs to numeric expressions. Assignment, arguments, bool,
    # Any and integer-only width/sign rules keep their existing strict types.
    cases = {
        "assign-integer-to-float": "x = 1.0\n    y = 2\n    x = y",
        "assign-float-to-integer": "x = 1\n    x = 2.0",
        "initialize-float-from-integer": "y = 2\n    x: f64 = y",
        "narrow-float": "x: f32 = 1.0\n    y: f64 = 2.0\n    x = x + y",
        "integer-width": "x: i32 = 1\n    y: i64 = 2\n    z = x + y",
        "integer-sign": "x: i64 = 1\n    y: u64 = 2\n    z = x + y",
        "bool-add": "x = true + 1.0",
        "bool-compare": "x = true < 1.0",
        "any-add": "x = Any(1) + 1.0",
        "any-compare": "x = Any(1) < 1.0",
        "float-power-assign": "x = 2.0\n    x **= 2",
    }
    for operator in ("&", "|", "^", "<<", ">>"):
        cases["float-bitwise-" + operator] = "x = 1.0 " + operator + " 2"
        cases["integer-bitwise-" + operator] = "x = 2 " + operator + " 1.0"
    for operator in ("+=", "-=", "*=", "/=", "%="):
        cases["integer-compound-" + operator] = "x = 2\n    x " + operator + " 1.5"
        cases["narrow-compound-" + operator] = (
            "x: f32 = 1.0\n    y: f64 = 2.0\n    x " + operator + " y")
    return cases


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-numeric-") as temporary:
        work = Path(temporary)
        cases = refusal_cases()
        for index, (name, body) in enumerate(cases.items()):
            source = work / f"refusal-{index}.as"
            source.write_text("# aether: 3.0\ndef main() -> None:\n    " + body + "\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                diagnostic["code"] == "AS3202" for diagnostic in report["diagnostics"]
            ), (name, result.returncode, report, result.stderr)

        source = work / "argument.as"
        source.write_text("# aether: 3.0\ndef consume(x: f64) -> None:\n    pass\n"
                          "def main() -> None:\n    y = 2\n    consume(y)\n")
        result = invoke([compiler, "check", source, "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(
            diagnostic["code"] == "AS3202" for diagnostic in report["diagnostics"]
        ), ("argument", result.returncode, report)

        ir = work / "mixed.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(ir, work / "mixed", optimization, "mixed numeric ok\n")

        source = work / "screenshot.as"
        source.write_text(SCREENSHOT)
        emit_ir(compiler, source, ir)
        for optimization in ("-O0", "-O2"):
            require_output(ir, work / "screenshot", optimization, "3\n")
    print(f"PASS mixed numeric: {len(cases) + 1} refusal cases, promotion fixture and "
          "screenshot program at O0/O2 ASan/UBSan")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-numeric-control-") as temporary:
        work = Path(temporary)
        ir = work / "mixed.ll"
        emit_ir(compiler, FIXTURE, ir)
        require_output(ir, work / "positive", "-O0", "mixed numeric ok\n")
        # Signed-to-unsigned mutation remains valid LLVM and links normally,
        # but converts -7 into a large positive number. A mere build/check gate
        # misses this defect; the identical fixture must fail an assertion.
        text, count = re.subn(r"\bsitofp\b", "uitofp", ir.read_text())
        assert count > 0, "signed conversion mutation anchor changed"
        ir.write_text(text)
        result = sanitized(ir, RUNTIME, work / "negative", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        assert "mixed numeric ok" not in result.stdout, result
        print("PASS negative control: unsigned conversion of signed operands "
              "observed failing with AssertionError")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
