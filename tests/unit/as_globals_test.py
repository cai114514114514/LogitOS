#!/usr/bin/env python3
"""Module binding, initialization and precise global roots on the native path.

The guest gate executes the same diamond-import fixture. Private IR mutations
make the root and initialization oracles fail; no product source is mutated.
"""

import argparse
import json
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/globals/main.as"


def source_file(work, name, body):
    path = work / (name + ".as")
    path.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
    return path


def exercise(compiler):
    cases = {
        "future": (
            """
            first = second
            second = 2
            """,
            "AS3206",
        ),
        "self": (
            """
            value: i64 = value
            """,
            "AS3206",
        ),
        "changed-type": (
            """
            value = 1
            def change() -> None:
                global value
                value = 'x'
            """,
            "AS3202",
        ),
        "unknown-global": (
            """
            def change() -> None:
                global absent
                absent = 1
            """,
            "AS3205",
        ),
        "parameter-global": (
            """
            value = 1
            def change(value: i64) -> None:
                global value
            """,
            "AS3200",
        ),
        "shadow-before-store": (
            """
            value = 1
            def change() -> None:
                print(value)
                value = 2
            """,
            "AS3206",
        ),
        "branch-shadow": (
            """
            value = 1
            def change(flag: bool) -> None:
                if flag:
                    print(value)
                else:
                    value = 2
            """,
            "AS3206",
        ),
        "void": (
            """
            def nothing() -> None:
                pass
            value = nothing()
            """,
            "AS3202",
        ),
        "duplicate": (
            """
            value = 1
            value = 2
            """,
            "AS3200",
        ),
        "unknown-initializer-type": (
            """
            value = read()
            def read() -> i64:
                return value
            """,
            "AS3201",
        ),
        "import-write": (
            """
            import dependency
            def change() -> None:
                dependency.value = 2
            """,
            "AS3400",
        ),
        "import-global": (
            """
            from dependency import value
            def change() -> None:
                global value
            """,
            "AS3205",
        ),
        "private-import": (
            """
            import dependency
            def read() -> i64:
                return dependency._private
            """,
            "AS3300",
        ),
        "function-conflict": (
            """
            value = 1
            def value() -> i64:
                return 2
            """,
            "AS3200",
        ),
        "import-conflict": (
            """
            import dependency
            dependency = 1
            """,
            "AS3300",
        ),
    }
    with tempfile.TemporaryDirectory(prefix="as-globals-") as temporary:
        work = Path(temporary)
        source_file(work, "dependency", "value = 1\n_private = 9\n")
        for name, (body, code) in cases.items():
            source = source_file(work, name, body)
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                diagnostic["code"] == code for diagnostic in report["diagnostics"]), (name, report)

        checked = invoke([compiler, "check", FIXTURE, "--json"])
        report = json.loads(checked.stdout)
        assert checked.returncode == 0, report
        variables = {symbol["name"]: symbol for symbol in report["symbols"]
                     if symbol["kind"] == "variable"}
        assert variables["message"]["type"] == "str", variables
        assert not any(symbol["name"] == "__as_initialize_module"
                       for symbol in report["symbols"]), report
        ir = work / "globals.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "globals", optimization)
            assert result.returncode == 0 and result.stdout == "module globals ok\n", result
        result = invoke([compiler, "test", FIXTURE, "--toolchain", RUNTIME])
        assert result.returncode == 0, result

        # A late-bound global read is legal to check if its type is declared,
        # but must trap at the actual read when called before initialization.
        for body in (
            "first = read()\nlater: i64 = 3\ndef read() -> i64:\n    return later\n",
            "first = read()\nlater: Array[i64, 1] = [3]\ndef read() -> i64:\n    return later[0]\n",
        ):
            source = source_file(work, "early-read", body + "def main() -> None:\n    print('unreachable')\n")
            emit_ir(compiler, source, ir)
            for optimization in ("-O0", "-O2"):
                result = sanitized(ir, RUNTIME, work / "early-read", optimization)
                assert result.returncode == 1 and "RuntimeError" in result.stderr, result
                assert not result.stdout, result
                assert "early-read.as:5" in result.stderr, result
    print(f"PASS native globals: {len(cases)} diagnostics, symbols, O0/O2 roots/order/traps and test initialization")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-global-controls-") as temporary:
        work = Path(temporary)
        source = source_file(work, "roots", "text = 'global ' + 'only'\ndef main() -> None:\n    gc_collect()\n    assert text == 'global only'\n")
        ir = work / "roots.ll"
        emit_ir(compiler, source, ir)
        result = sanitized(ir, RUNTIME, work / "roots-ok", "-O0")
        assert result.returncode == 0, result
        broken, count = re.subn(
            r"^  call void @at_gc_root\(ptr %root\d+, ptr @global\d+, ptr @scan\d+\)\n",
            "", ir.read_text(), flags=re.MULTILINE)
        assert count == 1, "global-root mutation anchor changed"
        ir.write_text(broken)
        result = sanitized(ir, RUNTIME, work / "roots-broken", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
        print("PASS negative control: missing global root observed as ASan use-after-free")

        source = source_file(work, "initialization", "first = read()\nlater: i64 = 7\ndef read() -> i64:\n    return later\ndef main() -> None:\n    print('incorrectly ran')\n")
        emit_ir(compiler, source, ir)
        result = sanitized(ir, RUNTIME, work / "order-ok", "-O0")
        assert result.returncode == 1 and "RuntimeError" in result.stderr, result
        original = ir.read_text()
        assert "internal global i1 false" in original
        ir.write_text(original.replace("internal global i1 false", "internal global i1 true"))
        result = sanitized(ir, RUNTIME, work / "order-broken", "-O0")
        assert result.returncode == 0 and result.stdout == "incorrectly ran\n", result
        print("PASS negative control: initialized-zero mutation observed bypassing required trap")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if args.negative_control:
        negative_controls(compiler)
    else:
        exercise(compiler)


if __name__ == "__main__":
    main()
