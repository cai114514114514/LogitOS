#!/usr/bin/env python3
"""Executable A2-to-A3 capability migration cases, with explicit gap boundaries.

These cases exercise native semantics and reject silent fallback. They are a
growing parity suite, not a declaration that the remaining VM is dispensable.
"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from as_runtime import copy_runtime

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "c/apps/as/runtime"
FIXTURES = ROOT / "tests/fixtures/astyped"
EXCEPTION_OUTPUT = ("ZeroDivisionError 2\n42 4\ncaught 2\nValueError: 中文错误\n"
                    "reraised 5\nelse\ndone\n")


def invoke(arguments, **kwargs):
    return subprocess.run(list(map(str, arguments)), text=True, capture_output=True,
                          timeout=90, **kwargs)


def run_native(compiler, path, debug=False, toolchain=RUNTIME):
    arguments = [compiler, "run", path, "--json", "--toolchain", toolchain]
    if debug:
        arguments.append("--debug")
    result = invoke(arguments)
    report = json.loads(result.stdout)
    assert report["phase"] == "run", report
    assert result.returncode == report["exit_code"], report
    return report


def require_exception_recovery(report):
    assert report["ok"] and report["exit_code"] == 0, "exception-recovery"
    assert report["output"] == EXCEPTION_OUTPUT, "exception-recovery"


def negative_control(compiler):
    # Change only a private runtime copy. The same positive assertion must see
    # catch entry fail to clear pending state; the working runtime stays intact.
    with tempfile.TemporaryDirectory(prefix="as-parity-negative-") as temporary:
        directory = Path(temporary)
        copy_runtime(directory)
        runtime = (RUNTIME / "exception.c").read_text()
        anchor = "at_failed = 0;"
        assert runtime.count(anchor) == 1, "exception-state mutation anchor changed"
        (directory / "exception.c").write_text(runtime.replace(anchor, "at_failed = 1;"))
        report = run_native(compiler, FIXTURES / "exceptions/main.as", toolchain=directory)
        try:
            require_exception_recovery(report)
        except AssertionError as error:
            assert str(error) == "exception-recovery", error
        else:
            raise AssertionError("Parity gate accepted uncleared exception state")
    print("PASS negative control: exception-recovery observed failing")


def check_errors(compiler):
    cases = {
        "bare-raise": ("def main() -> None:\n    raise\n", "AS3701"),
        "raise-value": ("def main() -> None:\n    raise 1\n", "AS3202"),
        "constructor": ("def main() -> None:\n    raise ValueError(1)\n", "AS3202"),
        "constructor-arity": ("def main() -> None:\n    raise ValueError()\n", "AS3204"),
        "unknown-catch": ("""def main() -> None:
    try:
        pass
    except old_binding:
        pass
""", "AS3700"),
        "hidden-catch": ("""def main() -> None:
    try:
        pass
    except Error:
        pass
    except ValueError:
        pass
""", "AS3700"),
        "try-without-handler": ("def main() -> None:\n    try:\n        pass\n", "AS3700"),
        "partial-initialization": ("""def main() -> None:
    try:
        value = 1 / 0
    except Error:
        print(value)
""", "AS3206"),
        "binding-not-always-initialized": ("""def main() -> None:
    try:
        pass
    except Error as error:
        pass
    print(error)
""", "AS3206"),
        "handler-missing-return": ("""def answer() -> i64:
    try:
        return 1 / 0
    except Error:
        pass
""", "AS3208"),
        "immutable-record": ("""def main() -> None:
    error = ValueError("x")
    error.code = 3
""", "AS3702"),
        "reserved-type": ("struct Error:\n    code: i64\n", "AS3200"),
        "conditional-types": ("def main() -> None:\n    value = 1 if True else 2.5\n", "AS3202"),
        "conditional-condition": ("def main() -> None:\n    value = 1 if 1 else 2\n", "AS3202"),
        "find-argument": ("def main() -> None:\n    print(\"abc\".find(1))\n", "AS3202"),
        "comparison-types": ("def main() -> None:\n    print(\"abc\" == 1)\n", "AS3202"),
    }
    for name, (source, code) in cases.items():
        result = invoke([compiler, "check", "--json", "--stdin", name + ".as"],
                        input="# aether: 3.0\n" + source)
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(d["code"] == code for d in report["diagnostics"]), (name, report)
    return len(cases)


def exercise(compiler):
    count = check_errors(compiler)
    with tempfile.TemporaryDirectory(prefix="as-native-parity-") as temporary:
        work = Path(temporary)
        control = work / "control.as"
        control.write_text("""# aether: 3.0
def make() -> Error:
    return ValueError("original")
def main() -> None:
    value: i64
    try:
        value = 1
    except Error:
        value = 2
    assert value == 1
    try:
        try:
            pass
        except Error:
            print("WRONG HANDLER")
        else:
            raise make()
    except ValueError as error:
        assert error.line == 17
        assert error.message == "original"
    try:
        try:
            raise ValueError("first")
        except ValueError:
            raise IOError("second")
        except IOError:
            print("WRONG HANDLER")
    except IOError as second:
        print(second.message)
    try:
        assert False
    except AssertionError:
        pass
    try:
        values: Array[i64, 1] = [7]
        print(values[1])
    except IndexError:
        pass
    try:
        i8(200)
    except ConversionError:
        pass
    try:
        1 << 64
    except ValueError:
        pass
    try:
        1 / 0
    except:
        print("control ok")
""")
        for debug in (False, True):
            require_exception_recovery(run_native(compiler, FIXTURES / "exceptions/main.as", debug))
            text = run_native(compiler, FIXTURES / "text/main.as", debug)
            assert text["ok"] and text["output"] == "Aether 中文 second\ntext ok\n", text
            controls = run_native(compiler, control, debug)
            assert controls["ok"] and controls["output"] == "second\ncontrol ok\n", controls
            failed = run_native(compiler, FIXTURES / "uncaught/main.as", debug)
            assert failed["exit_code"] == 1 and "main.as:4:" in failed["output"], failed
            assert "IOError: 读取失败" in failed["output"] and "UNREACHABLE" not in failed["output"], failed
            count += 5
    print(f"PASS {count} native capability parity checks")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if args.negative_control:
        negative_control(compiler)
    else:
        exercise(compiler)


if __name__ == "__main__":
    main()
