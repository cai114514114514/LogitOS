#!/usr/bin/env python3
"""Native dictionaries: type boundaries, mutation, rooting and forced collisions."""

import argparse
import json
from pathlib import Path
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME
from as_runtime import copy_runtime

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/dict/main.as"


def exercise(compiler):
    cases = {
        "empty": ("def main() -> None:\n    data = {}", "AS3900"),
        "key": ('def main() -> None:\n    data = {"a": 1}\n    data[1]', "AS3202"),
        "value": ('def main() -> None:\n    data = {"a": 1}\n    data["b"] = "x"', "AS3202"),
        "mixed": ('def main() -> None:\n    data = {1: 2, "x": 3}', "AS3202"),
        "unhashable": ('def main() -> None:\n    data = {1.5: 2}', "AS3601"),
        "bad-default": ('def main() -> None:\n    data = {"a": 1}\n    data.get("a", "x")', "AS3202"),
        "arity": ('def main() -> None:\n    data = {"a": 1}\n    data.keys(1)', "AS3204"),
        "void": ('def main() -> None:\n    data: Dict[str, None] = {}', "AS3202"),
        "borrow": ("""
            def retain(data: Dict[str, Slice[i64]], view: Slice[i64]) -> None:
                data["borrow"] = view
            """, "AS3400"),
        "constraint": ("""
            def lookup[K, V](data: Dict[K, V], key: K) -> V:
                return data[key]
            """, "AS3601"),
    }
    cases["missing-result-context"] = ("""
        def empty[K: Hashable]() -> Dict[K, bool]:
            return {}
        def main() -> None:
            data = empty()
        """, "AS3602")
    with tempfile.TemporaryDirectory(prefix="as-dict-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n") + "\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == code for item in report["diagnostics"]), (name, report)
        ir = work / "dict.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "dict", optimization)
            assert result.returncode == 0 and result.stdout == "native dictionary ok\n", result
            sets_ir = work / "sets.ll"
            emit_ir(compiler, ROOT / "tests/fixtures/astyped/sets/main.as", sets_ir)
            result = sanitized(sets_ir, RUNTIME, work / "sets", optimization)
            assert result.returncode == 0 and result.stdout == "native sets library ok\n", result
            copy_runtime(work)
            runtime_test = ROOT / "tests/unit/as_dict_runtime_test.c"
            # The shared sanitizer harness links each runtime unit. Put its
            # public header beside this standalone C test's private copy.
            source = work / "collisions.c"
            source.write_text(runtime_test.read_text())
            result = sanitized(source, work, work / "collisions", optimization)
            assert result.returncode == 0 and "collision chains ok" in result.stdout, result
    print(f"PASS native Dict: {len(cases)} diagnostics, O0/O2 GC, mutation, snapshots and collision chains")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-dict-control-") as temporary:
        work = Path(temporary)
        ir = work / "dict.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "dict-ok", "-O0")
        assert result.returncode == 0, result
        copy_runtime(work)
        source = work / "dict.c"
        text = source.read_text()
        anchor = "dict->value_scan(bucket + dict->value_offset);"
        assert text.count(anchor) == 1, "dictionary value scanner mutation anchor changed"
        source.write_text(text.replace(anchor, "(void)bucket;"))
        result = sanitized(ir, work, work / "dict-broken", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS negative control: missing Dict value scanner observed as ASan use-after-free")


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
