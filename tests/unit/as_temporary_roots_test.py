#!/usr/bin/env python3
"""Completed expression roots must release memory without dropping live owners."""

import argparse
import os
from pathlib import Path
import tempfile

from as_compiler import compiler_sources
from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/temporary-roots/main.as"


def exercise(compiler, work):
    ir = work / "main.ll"
    emit_ir(compiler, FIXTURE, ir)
    runtime = work / "runtime"
    copy_runtime(runtime)
    heap = runtime / "heap.c"
    text = heap.read_text()
    anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
    assert text.count(anchor) == 1
    heap.write_text(text.replace(anchor, anchor + "\n    at_gc_collect();"))
    for optimization in ("-O0", "-O2"):
        result = sanitized(ir, runtime, work / "program", optimization)
        assert result.returncode == 0 and result.stdout == "native temporary roots ok\n", result


def negative_control(compiler, work):
    exercise(compiler, work)
    original = ROOT / "c/apps/as/backend/llvm/roots.c"
    replacement = work / "roots.c"
    text = original.read_text()
    anchor = "void at_ir_release_temporaries(Gen *generator, AtNode *node)\n{"
    assert text.count(anchor) == 1
    replacement.write_text(text.replace(anchor, anchor + "\n    return;"))
    sources = [replacement if path == original else path for path in compiler_sources()]
    broken = work / "asc"
    result = invoke([os.environ.get("CC", "clang"), "-O1", *sources,
                     "-I", ROOT / "c/apps/as", "-I", ROOT / "include/abi", "-o", broken])
    assert result.returncode == 0, result.stderr
    ir = work / "retained.ll"
    emit_ir(broken, FIXTURE, ir)
    result = sanitized(ir, RUNTIME, work / "retained", "-O0")
    assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS temporary-root control: stale expression owners observed retaining excess memory")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-temporary-roots-") as temporary:
        (negative_control if args.negative_control else exercise)(args.compiler.resolve(), Path(temporary))
    if not args.negative_control:
        print("PASS temporary roots: bounded retained bytes, constructors, arguments, loops and handlers, O0/O2")
