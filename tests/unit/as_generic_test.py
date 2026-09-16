#!/usr/bin/env python3
"""Generic declarations, independent instances and real native execution.

The constraint negative control invokes exercise_constraints() first and must
fail at exactly 'unconstrained-body'. Positive compile/run cases also inspect
emitted function signatures, so silently boxing everything is not a pass.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/generics/main.as"


def run(arguments, **kwargs):
    return subprocess.run(list(map(str, arguments)), text=True, capture_output=True,
                          timeout=60, **kwargs)


def check(compiler, source):
    result = run([compiler, "check", "--json", "--stdin", "generic.as"], input="# aether: 3\n" + source)
    report = json.loads(result.stdout)
    assert result.returncode == (0 if report["ok"] else 1), result
    return report


def exercise_constraints(compiler):
    cases = {
        "unconstrained-body": ("""def add[T](left: T, right: T) -> T:
    return left + right
""", "AS3202"),
        "number-does-not-promise-bitwise": ("""def bits[T: Number](left: T, right: T) -> T:
    return left & right
""", "AS3202"),
        "unknown-protocol": ("""def identity[T: Missing](value: T) -> T:
    return value
""", "AS3601"),
        "duplicate-parameter": ("""def identity[T, T](value: T) -> T:
    return value
""", "AS3600"),
        "type-parameter-scope": ("""def identity[T](value: T) -> T:
    return value
def other(value: T) -> None:
    pass
""", "AS3200"),
        "constraint-mismatch": ("""def twice[T: Number](value: T) -> T:
    return value + value
def main() -> None:
    twice("text")
""", "AS3601"),
        "inconsistent-inference": ("""def choose[T](left: T, right: T) -> T:
    return left
def main() -> None:
    choose(1, 2.5)
""", "AS3602"),
        "unbound-parameter": ("""def constant[T](value: i64) -> i64:
    return value
def main() -> None:
    constant(1)
""", "AS3602"),
        "unconstrained-conversion": ("""def make[T](value: T) -> T:
    return T(1)
""", "AS3601"),
        "constraint-cannot-be-strengthened-by-call": ("""def integer[T: Integer](value: T) -> T:
    return value
def number[U: Number](value: U) -> U:
    return integer(value)
""", "AS3601"),
        "generic-cannot-hide-borrow-escape": ("""def identity[T](value: T) -> T:
    return value
def escape(values: Slice[i64]) -> None:
    identity(values)
""", "AS3400"),
    }
    for name, (source, code) in cases.items():
        report = check(compiler, source)
        assert any(d["code"] == code for d in report["diagnostics"]), name
    return len(cases)


def exercise(compiler, directory):
    checks = exercise_constraints(compiler)
    toolchain = ROOT / "c/apps/as/runtime"
    checked = run([compiler, "check", FIXTURE, "--json"])
    assert checked.returncode == 0, checked.stdout + checked.stderr
    report = json.loads(checked.stdout)
    identities = [symbol for symbol in report["symbols"] if symbol["name"] == "identity"]
    assert len(identities) == 1 and identities[0]["type_parameters"] == [{"name": "T", "constraint": ""}]
    checks += 2

    expected = "6 3.75\n8 3\n120 泛型\n"
    for debug in (False, True):
        args = [compiler, "run", FIXTURE, "--toolchain", toolchain, "--json"]
        if debug:
            args.append("--debug")
        executed = run(args)
        assert executed.returncode == 0, executed.stdout + executed.stderr
        assert json.loads(executed.stdout)["output"] == expected, executed.stdout
        checks += 2

    # Cache identity includes all type arguments. Repeated i64 calls reuse one
    # machine function; f64 gets another signature and another typed tree.
    reuse = directory / "reuse.as"
    reuse.write_text("""# aether: 3
def identity[T](value: T) -> T:
    return value
def main() -> None:
    print(identity(1), identity(2), identity(3.5))
""")
    lowered = run([compiler, "build", reuse, "--emit-llvm"])
    assert lowered.returncode == 0, lowered.stdout + lowered.stderr
    signatures = re.findall(r"^define (.+) @fn\d+\((.*)\)", lowered.stdout, re.M)
    assert sorted(signatures) == sorted([("void", ""), ("i64", "i64 %arg0"), ("double", "double %arg0")]), signatures
    checks += 1

    # Runtime failures must retain the original generic body's location and
    # short-circuit the caller for every width, in both optimization modes.
    for typename, maximum in (("i8", 127), ("u8", 255), ("i64", 9223372036854775807)):
        source = directory / f"overflow-{typename}.as"
        source.write_text(f"""# aether: 3
def twice[T: Integer](value: T) -> T:
    return value + value
def main() -> None:
    value: {typename} = {maximum}
    result = twice(value)
    print("UNREACHABLE", result)
""")
        for debug in (False, True):
            args = [compiler, "run", source, "--json", "--toolchain", toolchain]
            if debug:
                args.append("--debug")
            executed = run(args)
            result = json.loads(executed.stdout)
            assert executed.returncode == 1 and result["exit_code"] == 1, result
            assert f"overflow-{typename}.as:3:" in result["output"] and "OverflowError" in result["output"], result
            assert "UNREACHABLE" not in result["output"], result
            checks += 3

    print(f"PASS {checks} generic/native host checks")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-generic-") as directory:
        exercise(args.compiler.resolve(), Path(directory))


if __name__ == "__main__":
    main()
