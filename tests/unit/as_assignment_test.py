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
COMPOUND_OPS = ROOT / "tests/fixtures/astyped/compound-ops/main.as"
COMPOUND_OPS_OUTPUT = "native compound bitwise/shift/power ok\n"


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
            # A4-F3 parse-accept proof: all six new operators must build an
            # AN_ASSIGN (they fail only later, in sema, with the SAME numeric
            # diagnostic the old five produce). A lexer that split `**=` into
            # `**` `=`, or a parser range that stopped at T_PERCENTEQ, would
            # surface here as AS3100 instead -- absent beats present-and-wrong.
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
            # A4-F3 sema: the six new ops take the INTEGER constraint set, not
            # Number -- `f: f64; f **= 2.0` passed the generic numeric gate
            # before this rule and would have lowered T_POW onto the frem
            # branch of at_ir_numeric (a plausible wrong program). The binary
            # forms carry the identical refusal in sema/expression.c.
            "float-power": ("def main() -> None:\n    value: f64 = 2.0\n    value **= 2.0", "AS3202"),
            "float-shl": ("def main() -> None:\n    value: f64 = 2.0\n    value <<= 1", "AS3202"),
            "float-and": ("def main() -> None:\n    value: f64 = 2.0\n    value &= 2.0", "AS3202"),
            "float-shr": ("def main() -> None:\n    value: f64 = 2.0\n    value >>= 1", "AS3202"),
            # And the five old ops must NOT be refused: float += keeps working
            # (the new integer rule names its six operators, not "compound").
            "float-plus-ok": ("def main() -> None:\n    value: f64 = 2.0\n    value += 2.0", None),
            "str-power": ("def main() -> None:\n    value: str = \"ab\"\n    value **= 2", "AS3202"),
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
            if code is None:
                # Positive accept cases (float compound arithmetic stays legal)
                # share this table so one loop proves both halves of the rule.
                assert result.returncode == 0 and not report["diagnostics"], (name, report)
            else:
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
        emit_ir(compiler, COMPOUND_OPS, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "compound-ops", optimization)
            assert result.returncode == 0 and result.stdout == COMPOUND_OPS_OUTPUT, result
    # A4-F3 parse-accept proof: each new operator must build an AN_ASSIGN and
    # pass checking -- the binary forms `** << >> & | ^` already exist
    # (numeric.c lowers T_POW via checked_power and shifts via at_ir_shift),
    # so as_compound_operator routes the compound forms onto the same
    # lowering. A lexer that split `**=` into `**` `=`, or a parser range that
    # stopped at T_PERCENTEQ, would surface here as AS3100 instead -- absent
    # beats present-and-wrong. The F3-semantics edge rules landed with the
    # compound-ops fixture (run above): no-final-square (`i8: 100 **= 1`),
    # unsigned shift-count test (negative count refused by the same compare),
    # <<= round-trip OverflowError, >>= arithmetic/logical by signedness, and
    # the integer-only refusals in invalid_targets (float-power & friends).
    parse_accept = {
        "poweq": "count **= 2\n",
        "ampeq": "count &= 2\n",
        "pipeeq": "count |= 2\n",
        "careteq": "count ^= 2\n",
        "shleq": "count <<= 2\n",
        "shreq": "count >>= 2\n",
    }
    with tempfile.TemporaryDirectory(prefix="as-compound-accept-") as temporary:
        work = Path(temporary)
        for name, statement in parse_accept.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\ndef main() -> None:\n    count: i64 = 1\n    " + statement)
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            codes = [error["code"] for error in report["diagnostics"]]
            assert result.returncode == 0 and not codes, (name, report)
    print(f"PASS compound accept: {len(parse_accept)} operators parsed as AN_ASSIGN and checked")

    print(f"PASS assignment: {len(cases) + len(invalid_targets) + 2} diagnostics, locals/modules, atomic stores, O0/O2 ASan")
    print("PASS compound assignment: receiver/index order, old values, resizing, GC and failure ordering")
    print("PASS compound-ops: **= &= |= ^= <<= >>= edge rules (round-trip, unsigned count, no final square)")


def jar_control():
    """One jar, two doors: delete a line of as_compound_operator and BOTH doors
    must go red. The mutation recompiles the whole compiler with the lexer.h
    table missing its `**=` entry, then checks the compound-ops fixture.
    Backend door: `hundred: i8 = 100; hundred **= 1` falls to the unchanged-
    token fallback in compound.c and `at_ir_numeric(T_POWEQ, ...)` hits the
    checked-arithmetic `%`-else branch -- the stored value truncates to 0x64 *
    0x64 mod 256 == 16, so `assert hundred == 100` prints AssertionError, not
    a compile error. Eval door (watched separately, by hand, against
    test-as-eval): eval.c's fallback fails by name. If EITHER door computed
    the right answer here, the jar would have a second, drifting table."""
    original_path = ROOT / "c/apps/as/frontend/lexer.h"
    original = original_path.read_text()
    anchor = "    case T_POWEQ:\n        return T_POW;\n"
    assert original.count(anchor) == 1, "as_compound_operator anchor moved"
    mutated = original.replace(anchor, "")
    sources = compiler_sources()
    with tempfile.TemporaryDirectory(prefix="as-jar-control-") as temporary:
        work = Path(temporary)
        # The jar is a HEADER: as-host-sources lists no .h files, so swapping a
        # positional path in the compile line would compile the tree header
        # untouched and the "mutant" would be the real compiler -- a fake
        # control (found red: the fixture passed and the assert below fired).
        # Shade it through the include path instead: "frontend/lexer.h" resolves
        # against -I directories, so this directory must come FIRST.
        assert not any(path.suffix == ".h" for path in sources), \
            "as-host-sources gained headers; the -I shade would silently stop mattering"
        (work / "frontend").mkdir()
        (work / "frontend" / "lexer.h").write_text(mutated)
        compiler = work / "asc-mutant"
        command = [os.environ.get("CC", "clang"), "-O1",
                   "-I" + str(work),
                   "-I" + str(ROOT / "c/apps/as"), "-I" + str(ROOT / "include/abi"),
                   *sources,
                   "-o", compiler, "-lm"]
        result = invoke(command)
        assert result.returncode == 0, result.stderr
        ir = work / "compound-ops.ll"
        emit_ir(compiler, COMPOUND_OPS, ir)
        result = sanitized(ir, RUNTIME, work / "jar-mutant", "-O0")
        assert result.returncode != 0 and "AssertionError" in result.stderr, result
        assert result.stdout != COMPOUND_OPS_OUTPUT, result
    print("PASS jar control: a dropped **= line reddens the backend gate (AssertionError)")


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
                       "-o", compiler, "-lm"]
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
        jar_control()
