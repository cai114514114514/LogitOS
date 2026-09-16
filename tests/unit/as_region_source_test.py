#!/usr/bin/env python3
"""Source-level Region ownership and emitted cleanup, with observable controls.

Moved-owner controls only run the checker. No invalid program is executed; the
runtime control omits a free and fails an independent allocation balance check.
"""

import argparse
import json
import os
from pathlib import Path
import tempfile

from as_compiler import compiler_sources
from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_region_test import instrument

FIXTURE = ROOT / "tests/fixtures/astyped/region"


def source_file(work, name, body):
    path = work / (name + ".as")
    path.write_text("# aether: 3.0\n" + body + "\n")
    return path


def rejected(compiler, source, code):
    result = invoke([compiler, "check", source, "--json"])
    report = json.loads(result.stdout)
    assert result.returncode == 1 and any(
        item["code"] == code for item in report["diagnostics"]), (source.name, report)
    if code == "AS3402":
        for diagnostic in report["diagnostics"]:
            if diagnostic["code"] == code:
                assert diagnostic["help"] and diagnostic["source_checksum"] == report["source_checksum"]


def moved_cases():
    # The common prefix is a live owner and a successful nested move. Every
    # suffix observes the original owner, which must be rejected statically.
    prefix = """def main() -> None:
    with owner = region(8):
        with moved = owner.move():
            pass
"""
    cases = {
        "moved-read": prefix + "        value = owner[0]",
        "moved-write": prefix + "        owner[0] = 1",
        "moved-length": prefix + "        len(owner)",
        "moved-again": prefix + "        with again = owner.move():\n            pass",
        "branch": """def test(take: bool) -> None:
    with owner = region(1):
        if take:
            with moved = owner.move():
                pass
        owner[0] = 1
""",
        "loop": """def main() -> None:
    with owner = region(1):
        for iteration in range(3):
            with moved = owner.move():
                pass
""",
        "continue": """def main() -> None:
    with owner = region(1):
        for iteration in range(3):
            with moved = owner.move():
                continue
""",
        "condition": """def main() -> None:
    with owner = region(1):
        while len(owner) == 1:
            with moved = owner.move():
                pass
""",
        "break-join": """def main() -> None:
    with owner = region(1):
        for iteration in range(3):
            with moved = owner.move():
                break
        len(owner)
""",
        "catch": """def main() -> None:
    with owner = region(1):
        try:
            with moved = owner.move():
                raise ValueError("moved")
        except Error:
            owner[0] = 1
""",
        "nested-catch": """def main() -> None:
    with owner = region(1):
        try:
            try:
                with moved = owner.move():
                    raise ValueError("moved")
            except Error as error:
                raise error
        except Error:
            len(owner)
""",
        "catch-join": """def main() -> None:
    with owner = region(1):
        try:
            with moved = owner.move():
                pass
        except Error:
            pass
        len(owner)
""",
    }
    return cases


def setup_tracker(work):
    runtime = work / "runtime"
    instrument(runtime)
    (runtime / "tracker.c").write_text((FIXTURE / "tracker.c").read_text())
    manifest = runtime / "sources.def"
    manifest.write_text(manifest.read_text() + "AT_RUNTIME_SOURCE(tracker)\n")
    return runtime


def require_success(result, tracked=False):
    expected = "native region ownership ok\n"
    if tracked:
        expected += "native region balance ok\n"
    assert result.returncode == 0 and result.stdout == expected, result


def exercise(compiler, work):
    cases = {
        "unscoped": ("def main() -> None:\n    owner = region(8)", "AS3401"),
        "arity": ("def main() -> None:\n    with r = region():\n        pass", "AS3204"),
        "size-type": ("def main() -> None:\n    with r = region(1.0):\n        pass", "AS3202"),
        "argument": ("def use(r: Region) -> None:\n    pass", "AS3401"),
        "return-type": ("def use() -> Region:\n    with r = region(1):\n        return r", "AS3401"),
        "aggregate": ("struct Box:\n    value: Region", "AS3401"),
        "optional": ("def f(r: Optional[Region]) -> None:\n    pass", "AS3401"),
    }
    inner = {
        "copy": ("copy = r", "AS3401"),
        "any": ("boxed: Any = r", "AS3401"),
        "list": ("owners = [r]", "AS3401"),
        "reassign": ("r = buffer(1)", "AS3401"),
        "unscoped-move": ("copy = r.move()", "AS3401"),
        "move-arity": ("with moved = r.move(1):\n            pass", "AS3204"),
        "nested-acquisition": ("with moved = r:\n            pass", "AS3401"),
        "capture": ("def inside() -> i64:\n            return len(r)", "AS3401"),
        "escape-address": ("unsafe:\n            addr(r)", "AS3401"),
    }
    for name, (body, code) in inner.items():
        cases[name] = ("def main() -> None:\n    with r = region(8):\n        " + body, code)
    cases.update({name: (body, "AS3402") for name, body in moved_cases().items()})
    for name, (body, code) in cases.items():
        rejected(compiler, source_file(work, name, body), code)

    ir = work / "region.ll"
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
    print(f"PASS Region source: {len(cases)} diagnostics, moves, byte access, scope/loop/exception "
          "cleanup, real free balance, O0/O2 ASan/UBSan")


def controls(compiler, work):
    cases = {name: source_file(work, name, body) for name, body in moved_cases().items()}
    for source in cases.values():
        rejected(compiler, source, "AS3402")
    original = ROOT / "c/apps/as/sema/check.c"
    source = original.read_text()
    anchor = "at_check_region_flow(p);"
    assert source.count(anchor) == 1
    changed = work / "check.c"
    changed.write_text(source.replace(anchor, "/* deliberately omit ownership flow */"))
    mutant = work / "asc-no-ownership"
    result = invoke([os.environ.get("CC", "clang"), "-O1",
                     "-I" + str(ROOT / "c/apps/as"), "-I" + str(ROOT / "include/abi"),
                     *[changed if path == original else path for path in compiler_sources()],
                     "-o", mutant])
    assert result.returncode == 0, result.stderr
    for source in cases.values():
        result = invoke([mutant, "check", source, "--json"])
        assert result.returncode == 0 and json.loads(result.stdout)["ok"], result
    print(f"PASS Region control: disabled ownership flow incorrectly accepts {len(cases)} "
          "moved-owner programs; checker only, never executed")

    runtime = setup_tracker(work)
    ir = work / "region.ll"
    emit_ir(compiler, FIXTURE / "main.as", ir)
    require_success(sanitized(ir, runtime, work / "baseline", "-O0"), True)
    region = runtime / "region.c"
    source = region.read_text()
    anchor = "test_region_free(owner);"
    assert source.count(anchor) == 1
    region.write_text(source.replace(anchor, "/* deliberately omit free */"))
    result = sanitized(ir, runtime, work / "no-free", "-O0")
    assert result.returncode != 0 and "allocations == releases" in result.stderr, result
    print("PASS Region control: omitted free fails independent allocation balance assertion")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-region-source-") as directory:
        (controls if args.negative_control else exercise)(args.compiler.resolve(), Path(directory))


if __name__ == "__main__":
    main()
