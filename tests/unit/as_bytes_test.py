#!/usr/bin/env python3
"""Immutable binary snapshots, byte value equality and precise native roots."""

import argparse
import json
from pathlib import Path
import re
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/bytes/main.as"
EXPECTED = "native Bytes ok\n"


def exercise(compiler):
    cases = {
        "arity": ("Bytes()", "AS3204"),
        "extra": ('Bytes("a", "b")', "AS3204"),
        "integer": ("Bytes(3)", "AS3202"),
        "write": ('value = Bytes("ab")\n    value[0] = 1', "AS3400"),
        "compound": ('value = Bytes("ab")\n    value[0] += 1', "AS3400"),
        "temporary": ('Bytes("ab")[0] = 1', "AS3400"),
        "mutable-alias": ('value: Buffer = Bytes("a")', "AS3202"),
        "text-alias": ('value: str = Bytes("a")', "AS3202"),
        "float-index": ('value = Bytes("a")[1.0]', "AS3202"),
        "byte-type": ('value: str = Bytes("a")[0]', "AS3202"),
        "ordered": ('value = Bytes("a") < Bytes("b")', "AS3202"),
    }
    with tempfile.TemporaryDirectory(prefix="as-bytes-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\ndef main() -> None:\n    " + body + "\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), (name, report)
        ir = work / "bytes.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "bytes", optimization)
            assert result.returncode == 0 and result.stdout == EXPECTED, result
    print(f"PASS Bytes: {len(cases)} diagnostics; snapshots, equality, UTF-8 bytes, containers and GC at O0/O2 ASan")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-bytes-controls-") as temporary:
        work = Path(temporary)
        ir = work / "bytes.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "positive", "-O0")
        assert result.returncode == 0 and result.stdout == EXPECTED, result
        copy_runtime(work)

        # Break the actual copy, then value equality, in private runtime copies.
        # Either defect must reach a language assertion, not merely fail linking.
        original = (RUNTIME / "buffer.c").read_text()
        mutations = {
            "copy": ("memcpy(bytes->data, data, (size_t)length);", ";"),
            "equality": ("return left->length == right->length &&",
                         "return left == right && left->length == right->length &&"),
        }
        for name, (anchor, replacement) in mutations.items():
            assert original.count(anchor) == 1, (name, "mutation anchor changed")
            (work / "buffer.c").write_text(original.replace(anchor, replacement))
            result = sanitized(ir, work, work / name, "-O0")
            assert result.returncode == 1 and "AssertionError" in result.stderr, result
        (work / "buffer.c").write_text(original)

        # A Bytes-only program isolates the compiler's root registration from
        # the many other managed types exercised by the full fixture.
        source = work / "roots.as"
        source.write_text('''# aether: 3.0
def make() -> Bytes:
    return Bytes("abc")
def main() -> None:
    value = make()
    gc_collect()
    assert value[0] == 97
''')
        emit_ir(compiler, source, ir)
        result = sanitized(ir, RUNTIME, work / "roots-positive", "-O0")
        assert result.returncode == 0, result
        changed, count = re.subn(r"^  call void @at_gc_root\([^\n]+\n", "", ir.read_text(),
                                 flags=re.MULTILINE)
        assert count > 0, "Bytes root mutation anchor changed"
        ir.write_text(changed)
        result = sanitized(ir, RUNTIME, work / "roots-negative", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS Bytes controls: missing copy, pointer equality and missing roots observed failing")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
