#!/usr/bin/env python3
"""Native routing of the existing `as FILE ARGS` launcher contract."""

import argparse
import os
from pathlib import Path
import tempfile

from as_compiler import compiler_sources
from as_managed_test import ROOT, invoke


def source_file(work):
    source = work / "program with spaces.as"
    source.write_text('''# aether: 3.0
def main() -> i64:
    assert port_stats()["open"] == 0
    with owner = region(1):
        with moved = owner.move():
            with view = moved.borrow_mut(0, 1):
                view[0] = 7
            assert moved[0] == 7 and len(moved) == 1
    unsafe:
        storage = alloc(4)
        pointer = i32ptr(addr(storage))
        pointer[0] = 7
        assert pointer[0] == 7
        dealloc(storage)
    command = run("echo", "native CLI child") |> run("cat")
    assert command.out() == "native CLI child\\n" and command.wait() == 0
    arguments = args()
    for index in range(1, len(arguments)):
        print("argument", index, arguments[index])
    return 7
''')
    return source


def exercise(compiler, work):
    source = source_file(work)
    arguments = ["", "two words", "中文", "--json", "--debug"]
    expected = "argument 1 \nargument 2 two words\nargument 3 中文\nargument 4 --json\nargument 5 --debug\n"
    result = invoke([compiler, source, *arguments])
    assert result.returncode == 7 and result.stdout == expected, result
    explicit = invoke([compiler, "run", source, "--", *arguments])
    assert (explicit.returncode, explicit.stdout) == (result.returncode, result.stdout), explicit
    # A shorthand argument cannot silently pretend to narrow authority. The
    # explicit run/-- form still permits arbitrary program data by agreement.
    result = invoke([compiler, source, "--scope", str(work)])
    assert result.returncode == 2 and not result.stdout and "refusing to run" in result.stderr, result
    bad = work / "bad.as"
    bad.write_text("# aether: 3.0\ndef main() -> None:\n    value: i64 = \"wrong\"\n")
    result = invoke([compiler, bad])
    assert result.returncode == 1 and "AS3202" in result.stdout + result.stderr, result
    assert not list(work.glob("*.la")), "Native shorthand emitted retiring bytecode"
    print("PASS native CLI: existing launcher, exact arguments/exit, type errors and scope refusal")


def negative_control(compiler, work):
    exercise(compiler, work)
    original = ROOT / "c/apps/as/cli/commands.c"
    text = original.read_text()
    anchor = "if (!native) {"
    assert text.count(anchor) == 1
    changed = work / "commands.c"
    changed.write_text(text.replace(anchor, "if (1) {"))
    sources = [changed if path == original else path for path in compiler_sources()]
    broken = work / "asc"
    result = invoke([os.environ.get("CC", "clang"), "-O1", *sources,
                     "-I", ROOT / "c/apps/as", "-I", ROOT / "include/abi", "-o", broken])
    assert result.returncode == 0, result
    result = invoke([broken, source_file(work), "proof"])
    assert result.returncode != 7 and "argument 1 proof" not in result.stdout, result
    assert "cannot run in the A2 VM" in result.stdout + result.stderr, result
    print("PASS CLI control: removed native routing sends the existing launcher to a refusing VM")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-native-cli-") as temporary:
        (negative_control if args.negative_control else exercise)(args.compiler.resolve(), Path(temporary))
