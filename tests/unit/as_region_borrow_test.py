#!/usr/bin/env python3
"""Scoped Region views: conflict/escape diagnostics and real native cleanup.

Compiler mutants only check invalid programs; none are executed. The runtime
control omits a descriptor free and must fail an independent allocation count.
"""

import argparse
import json
import os
from pathlib import Path
import tempfile
import textwrap

from as_compiler import compiler_sources
from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_region_source_test import source_file, setup_tracker

FIXTURE = ROOT / "tests/fixtures/astyped/region-borrow"
OUTPUT = "native region borrows ok\n"


def owner_scope(body):
    return "def main() -> None:\n    with owner = region(8):\n" + textwrap.indent(body, "        ")


def conflict_cases():
    bodies = {
        "shared-write": "with view = owner.borrow(0, 2):\n    owner[0] = 1",
        "shared-compound": "with view = owner.borrow(0, 2):\n    owner[0] += 1",
        "shared-move": "with view = owner.borrow(0, 2):\n    with moved = owner.move():\n        pass",
        "shared-exclusive": "with view = owner.borrow(0, 2):\n    with second = owner.borrow_mut(2, 4):\n        pass",
        "exclusive-read": "with view = owner.borrow_mut(0, 2):\n    owner[0]",
        "exclusive-write": "with view = owner.borrow_mut(0, 2):\n    owner[0] = 1",
        "exclusive-shared": "with view = owner.borrow_mut(0, 2):\n    with second = owner.borrow(2, 4):\n        pass",
        "exclusive-exclusive": "with view = owner.borrow_mut(0, 2):\n    with second = owner.borrow_mut(2, 4):\n        pass",
        "parent-read": "with view = owner.borrow_mut(0, 4):\n    with child = view.borrow(0, 1):\n        view[0]",
        "parent-write": "with view = owner.borrow_mut(0, 4):\n    with child = view.borrow(0, 1):\n        view[0] = 1",
        "parent-iterate": "with view = owner.borrow_mut(0, 4):\n    with child = view.borrow(0, 1):\n        for byte in view:\n            pass",
        "parent-comprehension": "with view = owner.borrow_mut(0, 4):\n    with child = view.borrow(0, 1):\n        values = [byte for byte in view]",
        "parent-unpack": "with view = owner.borrow_mut(0, 2):\n    with child = view.borrow(0, 1):\n        a, b = view",
        "parent-snapshot": "with view = owner.borrow_mut(0, 2):\n    with child = view.borrow(0, 1):\n        Bytes(view)",
        "parent-format": "with view = owner.borrow_mut(0, 2):\n    with child = view.borrow(0, 1):\n        str(view)",
        "parent-membership": "with view = owner.borrow_mut(0, 2):\n    with child = view.borrow(0, 1):\n        1 in view",
        "parent-print": "with view = owner.borrow_mut(0, 2):\n    with child = view.borrow(0, 1):\n        print(view)",
        "mutable-child": "with view = owner.borrow_mut(0, 4):\n    with child = view.borrow_mut(0, 1):\n        with sibling = view.borrow(1, 2):\n            pass",
        "shared-child": "with view = owner.borrow_mut(0, 4):\n    with child = view.borrow(0, 1):\n        with sibling = view.borrow_mut(1, 2):\n            pass",
        "handler": "with view = owner.borrow(0, 2):\n    try:\n        raise ValueError(\"x\")\n    except Error:\n        owner[0] = 1",
        "loop": "with view = owner.borrow(0, 2):\n    for i in range(2):\n        owner[0] = i",
        "branch": "with view = owner.borrow(0, 2):\n    if len(view) == 2:\n        owner[0] = 1",
    }
    cases = {name: owner_scope(body) for name, body in bodies.items()}
    cases["call-alias"] = """def update(first: MutSlice[u8], second: MutSlice[u8]) -> None:
    first[0] = second[0]
def main() -> None:
    with owner = region(2):
        with view = owner.borrow_mut(0, 2):
            update(view, view)
"""
    cases["later-argument"] = """def update(view: MutSlice[u8], value: u8) -> None:
    view[0] = value
def main() -> None:
    with owner = region(2):
        with view = owner.borrow_mut(0, 2):
            update(view, view[0])
"""
    cases["indirect-alias"] = """def update(first: MutSlice[u8], second: MutSlice[u8]) -> None:
    first[0] = second[0]
def main() -> None:
    callback: Callable[[MutSlice[u8], MutSlice[u8]], None] = update
    with owner = region(2):
        with view = owner.borrow_mut(0, 2):
            callback(view, view)
"""
    cases["parameter-alias"] = """def update(first: MutSlice[u8], second: MutSlice[u8]) -> None:
    first[0] = second[0]
def duplicate(view: MutSlice[u8]) -> None:
    update(view, view)
"""
    return cases


def escape_cases():
    bodies = {
        "copy": "with view = owner.borrow(0, 2):\n    escaped = view\nprint(len(escaped))",
        "mutable-copy": "with view = owner.borrow_mut(0, 2):\n    alias = view",
        "aggregate": "with view = owner.borrow(0, 2):\n    values = [view]",
        "any": "with view = owner.borrow(0, 2):\n    boxed = Any(view)",
        "capture": "with view = owner.borrow(0, 2):\n    def read() -> u8:\n        return view[0]",
        "rebind": "with view = owner.borrow(0, 2):\n    view = [u8(1), u8(2)]",
        "unpack-rebind": "with view = owner.borrow(0, 2):\n    view, other = [1, 2]",
        "catch-rebind": "with view = owner.borrow(0, 2):\n    try:\n        pass\n    except Error as view:\n        pass",
        "loop-rebind": "with view = owner.borrow(0, 2):\n    for view in [1]:\n        pass",
        "unscoped": "view = owner.borrow(0, 2)",
        "move-view": "with view = owner.borrow(0, 2):\n    with moved = view.move():\n        pass",
    }
    return {name: owner_scope(body) for name, body in bodies.items()}


def reject(compiler, source, code):
    result = invoke([compiler, "check", source, "--json"])
    report = json.loads(result.stdout)
    assert result.returncode == 1 and any(
        diagnostic["code"] == code for diagnostic in report["diagnostics"]), (source.name, report)
    if source.stem in conflict_cases():
        diagnostic = next(item for item in report["diagnostics"] if item["code"] == code)
        assert diagnostic["help"] and len(diagnostic["related"]) == 1, diagnostic
        related = diagnostic["related"][0]
        assert related["path"] == str(source) and related["source_checksum"] == report["source_checksum"]
        assert 0 <= related["start"] < related["end"] <= len(source.read_bytes()), related


def require_success(result, tracked=False):
    expected = OUTPUT + ("native region balance ok\n" if tracked else "")
    assert result.returncode == 0 and result.stdout == expected, result


def exercise(compiler, work):
    cases = {name: (body, "AS3403") for name, body in conflict_cases().items()}
    cases.update({name: (body, "AS3401") for name, body in escape_cases().items()})
    cases.update({
        "readonly-write": (owner_scope("with view = owner.borrow(0, 2):\n    view[0] = 1"), "AS3400"),
        "readonly-upgrade": (owner_scope("with view = owner.borrow(0, 2):\n    with child = view.borrow_mut(0, 1):\n        pass"), "AS3403"),
        "arity": (owner_scope("with view = owner.borrow(0):\n    pass"), "AS3204"),
        "offset-type": (owner_scope("with view = owner.borrow(0.0, 1):\n    pass"), "AS3202"),
        "mutable-parameter-copy": ("def update(view: MutSlice[u8]) -> None:\n    copied = view", "AS3401"),
        "view-result": ("def bad() -> Slice[u8]:\n    with owner = region(1):\n        with view = owner.borrow(0, 1):\n            return view", "AS3401"),
        "parameter-reborrow": ("def bad(view: Slice[u8]) -> None:\n    with child = view.borrow(0, 1):\n        pass", "AS3403"),
        "moved-owner": (owner_scope("with moved = owner.move():\n    pass\nwith view = owner.borrow(0, 1):\n    pass"), "AS3402"),
    })
    for name, (body, code) in cases.items():
        reject(compiler, source_file(work, name, body), code)
    ir = work / "borrow.ll"
    emit_ir(compiler, FIXTURE / "main.as", ir)
    runtime = setup_tracker(work)
    for optimization in ("-O0", "-O2"):
        require_success(sanitized(ir, RUNTIME, work / "production", optimization))
        require_success(sanitized(ir, runtime, work / "tracked", optimization), True)
    emit_ir(compiler, FIXTURE / "failure.as", ir)
    for optimization in ("-O0", "-O2"):
        result = sanitized(ir, runtime, work / "failure", optimization)
        assert result.returncode == 1 and "IndexError" in result.stderr, result
        assert str(FIXTURE / "failure.as") + ":5:" in result.stderr, result
        assert result.stdout == "native region balance ok\n", result
    print(f"PASS Region borrows: {len(cases)} diagnostics, nested shared/exclusive views, "
          "checked calls/generics, loop/exception cleanup, real frees, O0/O2 ASan/UBSan")


def compiler_mutant(work, original, anchor, replacement, name):
    source = original.read_text()
    assert source.count(anchor) == 1
    changed = work / original.name
    changed.write_text(source.replace(anchor, replacement))
    binary = work / name
    result = invoke([os.environ.get("CC", "clang"), "-O1",
                     "-I" + str(ROOT / "c/apps/as"), "-I" + str(ROOT / "include/abi"),
                     *[changed if path == original else path for path in compiler_sources()],
                     "-o", binary])
    assert result.returncode == 0, result.stderr
    return binary


def controls(compiler, work):
    cases = {name: source_file(work, name, body) for name, body in conflict_cases().items()}
    for source in cases.values():
        reject(compiler, source, "AS3403")
    mutant = compiler_mutant(work, ROOT / "c/apps/as/sema/check.c",
                             "at_check_region_borrows(p);", "/* omit loan conflict checking */",
                             "no-conflicts")
    for source in cases.values():
        result = invoke([mutant, "check", source, "--json"])
        assert result.returncode == 0 and json.loads(result.stdout)["ok"], result
    print(f"PASS borrow control: missing conflict analysis wrongly accepts {len(cases)} cases; check only")

    source = source_file(work, "escape", escape_cases()["copy"])
    reject(compiler, source, "AS3401")
    mutant = compiler_mutant(work, ROOT / "c/apps/as/sema/expression.c",
                             "checker->f->locals[node->symbol].scoped_borrow",
                             "0", "no-escape-check")
    result = invoke([mutant, "check", source, "--json"])
    assert result.returncode == 0 and json.loads(result.stdout)["ok"], result
    print("PASS borrow control: missing scoped-view escape check wrongly accepts an escaping alias; check only")

    runtime = setup_tracker(work)
    ir = work / "borrow.ll"
    emit_ir(compiler, FIXTURE / "main.as", ir)
    require_success(sanitized(ir, runtime, work / "baseline", "-O0"), True)
    borrow = runtime / "region_borrow.c"
    source = borrow.read_text()
    anchor = "test_region_free(borrow);"
    assert source.count(anchor) == 1
    borrow.write_text(source.replace(anchor, "/* omit descriptor free */"))
    result = sanitized(ir, runtime, work / "no-free", "-O0")
    assert result.returncode != 0 and "allocations == releases" in result.stderr, result
    print("PASS borrow control: missing descriptor free fails independent allocation balance")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-region-borrow-") as directory:
        (controls if args.negative_control else exercise)(args.compiler.resolve(), Path(directory))


if __name__ == "__main__":
    main()
