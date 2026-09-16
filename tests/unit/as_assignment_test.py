#!/usr/bin/env python3
"""Multiple assignment reads all right sides before initializing/storing targets."""

import argparse
import json
import os
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_compiler import compiler_sources

FIXTURE = ROOT / "tests/fixtures/astyped/unpack/main.as"
COMPOUND = ROOT / "tests/fixtures/astyped/compound/main.as"
COMPOUND_OUTPUT = "3\nnative compound assignment ok\n"


def exercise(compiler):
    cases = {
        "count": ("a, b = 1, 2, 3", "AS3204"),
        "uninitialized": ("a, b = 1, a", "AS3206"),
        "mismatch": ('a = 1\n    a, b = "x", 2', "AS3202"),
        "not-sequence": ("a, b = 2", "AS3202"),
        "array-count": ("values: Array[i64, 3] = [1, 2, 3]\n    a, b = values", "AS3204"),
        "void": ("a, b = print(1), 2", "AS3202"),
        "incomplete": ("a, b =", "AS3100"),
        "missing-name": ("a, = [1]", "AS3100"),
        "target-limit": (", ".join("a" + str(i) for i in range(65)) + " = range(65)", "AS3100"),
    }
    with tempfile.TemporaryDirectory(prefix="as-assignment-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\ndef main() -> None:\n    " + body + "\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), (name, report)
        for body, code in (("a, b = 1, a", "AS3206"), ("a, a = 1, 2", "AS3200")):
            source = work / "module.as"
            source.write_text("# aether: 3.0\n" + body + "\ndef main() -> None:\n    pass\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), report
        invalid_targets = {
            "temporary-struct": ("""
                struct Item:
                    value: i64
                def main() -> None:
                    Item(1).value += 2
                """, "AS3400"),
            "temporary-array": ("""
                def values() -> Array[i64, 2]:
                    return [1, 2]
                def main() -> None:
                    values()[0] += 1
                """, "AS3400"),
            "readonly-slice": ("""
                def change(values: Slice[i64]) -> None:
                    values[0] += 1
                """, "AS3400"),
            "returned-string": ("""
                def text() -> str:
                    return "ab"
                def main() -> None:
                    text()[0] = "x"
                """, "AS3400"),
            "uninitialized-module": ("count: i64\ncount += 1\n", "AS3200"),
            "uninitialized-local": ("""
                def main() -> None:
                    count: i64
                    count += 1
                """, "AS3206"),
        }
        for name, (body, code) in invalid_targets.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), (name, report)
        ir = work / "assignment.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "assignment", optimization)
            assert result.returncode == 0 and result.stdout == "native multiple assignment ok\n", result
        emit_ir(compiler, FIXTURE.parent.parent / "unpack-module/main.as", ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "module", optimization)
            assert result.returncode == 0 and result.stdout == "native module unpack ok\n", result
        emit_ir(compiler, COMPOUND, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "compound", optimization)
            assert result.returncode == 0 and result.stdout == COMPOUND_OUTPUT, result
    print(f"PASS assignment: {len(cases) + len(invalid_targets) + 2} diagnostics, locals/modules, atomic stores, O0/O2 ASan")
    print("PASS compound assignment: receiver/index order, old values, resizing, GC and failure ordering")


def compound_controls():
    original = ROOT / "c/apps/as/backend/llvm/compound.c"
    source = original.read_text()
    right = "    Val right = at_ir_expression(g, node->b);\n"
    assert source.count(right) == 1
    reordered = source.replace(right, "").replace("    int used = 0;\n", right + "    int used = 0;\n", 1)
    mutations = {
        "right-first": (reordered, "order"),
        "stale-slot": (source.replace("Val address = locate_place(g, place, 1);",
                                      "Val address = place->initial_address;"), "memory"),
        "unrooted-old-text": (source.replace("if (at_ir_references(g, previous.type)) {",
                                             "if (0) {"), "memory"),
    }
    sources = compiler_sources()
    with tempfile.TemporaryDirectory(prefix="as-compound-control-") as temporary:
        work = Path(temporary)
        for name, (mutated, failure) in mutations.items():
            assert mutated != source, f"missing mutation anchor: {name}"
            mutant = work / "compound.c"
            mutant.write_text(mutated)
            compiler = work / "asc"
            command = [os.environ.get("CC", "clang"), "-O1",
                       "-I" + str(ROOT / "c/apps/as"), "-I" + str(ROOT / "include/abi"),
                       *[mutant if path == original else path for path in sources],
                       "-o", compiler]
            result = invoke(command)
            assert result.returncode == 0, result.stderr
            ir = work / "compound.ll"
            emit_ir(compiler, COMPOUND, ir)
            result = sanitized(ir, RUNTIME, work / name, "-O0")
            if failure == "order":
                assert result.returncode == 1 and result.stdout == "12\n", result
                assert "AssertionError" in result.stderr, result
            else:
                assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
            print(f"PASS compound control: {name} observed failing")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-assignment-control-") as temporary:
        work = Path(temporary)
        ir = work / "assignment.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "positive", "-O0")
        assert result.returncode == 0, result
        text, count = re.subn(
            r"^  call void @at_gc_root\(ptr %root\d+, ptr %rootvalue\d+, ptr @scan\d+\)\n",
            "", ir.read_text(), flags=re.MULTILINE)
        assert count > 0, "parallel RHS root mutation anchor changed"
        ir.write_text(text)
        result = sanitized(ir, RUNTIME, work / "negative", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS assignment control: missing earlier RHS roots fail under ASan")
    compound_controls()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
