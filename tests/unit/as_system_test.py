#!/usr/bin/env python3
"""Native system boundary: word ABI, text ownership, authority and platform refusal."""

import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_runtime import copy_runtime, runtime_sources

FIXTURES = ROOT / "tests/fixtures/astyped/native-system"


def diagnostics(compiler, work):
    cases = {
        "unsafe-syscall": ("syscall(0)", "AS3810"),
        "unsafe-text": ('mem2str(Bytes("x"), 1)', "AS3810"),
        "arity-zero": ("unsafe:\n    syscall()", "AS3204"),
        "arity-five": ("unsafe:\n    syscall(0, 1, 2, 3, 4)", "AS3204"),
        "number-type": ('unsafe:\n    syscall("0")', "AS3202"),
        "argument-type": ('unsafe:\n    syscall(0, "x")', "AS3202"),
        "small-word": ('unsafe:\n    word: i32 = 1\n    syscall(0, word)', "AS3202"),
        "text-arity": ('unsafe:\n    mem2str(Bytes("x"))', "AS3204"),
        "text-address": ('unsafe:\n    mem2str("x", 1)', "AS3202"),
        "text-signed-address": ('unsafe:\n    pointer: i64 = 1\n    mem2str(pointer, 1)', "AS3202"),
        "text-length": ('unsafe:\n    mem2str(Bytes("x"), 1.0)', "AS3202"),
        "cstr-arity": ('unsafe:\n    mem2cstr()', "AS3204"),
        "lexical-function": ('unsafe:\n    def call() -> i64:\n        return syscall(0)', "AS3810"),
    }
    for name, (body, code) in cases.items():
        source = work / (name + ".as")
        source.write_text("# aether: 3\ndef main() -> None:\n" +
                          "\n".join("    " + line for line in body.splitlines()) + "\n")
        result = invoke([compiler, "check", source, "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(
            item["code"] == code for item in report["diagnostics"]), (name, report)


def runtime_probe(runtime, work, name):
    binary = work / name
    result = invoke([os.environ.get("CLANG", "clang"), "-O0", "-g",
                     "-fsanitize=address,undefined", "-I", runtime,
                     ROOT / "tests/fixtures/astyped/system-runtime.c",
                     *runtime_sources(runtime), "-o", binary])
    assert result.returncode == 0, result
    return invoke([binary], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1"))


def word_runtime(work):
    runtime = work / "words-runtime"
    copy_runtime(runtime)
    source = runtime / "system.c"
    original = source.read_text()
    anchor = "return AT_E_RUNTIME;"
    assert original.count(anchor) == 1
    source.write_text(original.replace(anchor,
        "extern int64_t probe_system_call(int64_t, uint64_t, uint64_t, uint64_t);\n"
        "    *out = probe_system_call(number, a, b, c);\n    return 0;"))
    shutil.copyfile(ROOT / "tests/fixtures/astyped/system-words.c", runtime / "system_words.c")
    manifest = runtime / "sources.def"
    manifest.write_text(manifest.read_text() + "\nAT_RUNTIME_SOURCE(system_words)\n")
    return runtime


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-native-system-") as temporary:
        work = Path(temporary)
        diagnostics(compiler, work)
        ir = work / "memory.ll"
        emit_ir(compiler, FIXTURES / "memory.as", ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "memory", optimization)
            assert result.returncode == 0 and result.stdout == "native memory text ok\n", result
        result = runtime_probe(RUNTIME, work, "runtime")
        assert result.returncode == 0, result

        # Actual host behavior is a typed error, including source location.
        # The private word transport below is explicitly not host OS support.
        source = work / "host.as"
        source.write_text("# aether: 3\ndef main() -> None:\n    unsafe:\n        syscall(0)\n")
        emit_ir(compiler, source, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "host", optimization)
            assert result.returncode == 1 and "RuntimeError" in result.stderr, result
            assert str(source) in result.stderr, result

        source = ROOT / "fsroot/as/examples/sys.as"
        emit_ir(compiler, source, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "sys-example", optimization)
            assert result.returncode == 1 and "RuntimeError" in result.stderr, result
            assert result.stdout == "" and str(source) in result.stderr, result

        runtime = word_runtime(work)
        emit_ir(compiler, FIXTURES / "words.as", ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, runtime, work / "words", optimization)
            assert result.returncode == 0 and result.stdout == "native syscall words ok\n", result
    print("PASS system: 13 diagnostics, O0/O2 ABI words, UTF-8 snapshots, bounds and host refusal")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-system-controls-") as temporary:
        work = Path(temporary)
        runtime = work / "runtime"
        copy_runtime(runtime)
        result = runtime_probe(runtime, work, "positive")
        assert result.returncode == 0, result
        mutations = [
            ("system.c", "if (!at_caps_have(AS_CAP_RAW))", "if (0)", "syscall-authority"),
            ("memory.c", "if (!at_caps_have(AS_CAP_RAW))", "if (0)", "memory-authority"),
            ("memory.c", "capacity >= 0 && length > capacity", "0", "memory-bound"),
        ]
        for filename, anchor, replacement, name in mutations:
            original = (RUNTIME / filename).read_text()
            assert original.count(anchor) == 1, name
            (runtime / filename).write_text(original.replace(anchor, replacement))
            result = runtime_probe(runtime, work, name)
            assert result.returncode != 0 and "assert" in result.stderr.lower(), result
            (runtime / filename).write_text(original)
            print("PASS system control:", name, "observed assertion failure")

        # Force collection at every allocation. Input arguments and resulting
        # text must remain alive across later allocation and explicit collection.
        heap = runtime / "heap.c"
        original = heap.read_text()
        anchor = "if (live_bytes + bytes >= collect_at)"
        assert original.count(anchor) == 1
        heap.write_text(original.replace(anchor, "if (1)"))
        ir = work / "memory.ll"
        emit_ir(compiler, FIXTURES / "memory.as", ir)
        result = sanitized(ir, runtime, work / "gc-positive", "-O0")
        assert result.returncode == 0 and result.stdout == "native memory text ok\n", result

        runtime = word_runtime(work)
        emit_ir(compiler, FIXTURES / "words.as", ir)
        result = sanitized(ir, runtime, work / "words-positive", "-O0")
        assert result.returncode == 0, result
        source = runtime / "system.c"
        original = source.read_text()
        anchor = "probe_system_call(number, a, b, c)"
        assert original.count(anchor) == 1
        source.write_text(original.replace(anchor, "probe_system_call(number, a, c, b)"))
        result = sanitized(ir, runtime, work / "wrong-words", "-O0")
        assert result.returncode != 0 and "assert" in result.stderr.lower(), result
        print("PASS system control: swapped ABI words observed assertion failure")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
