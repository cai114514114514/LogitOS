#!/usr/bin/env python3
"""Scoped native owners: diagnostics, real descriptors and observed controls."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime, runtime_sources

FIXTURE = ROOT / "tests/fixtures/astyped/ports/main.as"
FAULTS = ROOT / "tests/fixtures/astyped/port-runtime.c"


def diagnostics(compiler, work):
    cases = {
        "stats-arity": ('port_stats(1)', "AS3204"),
        "unscoped": ('file = open("x")', "AS3401"),
        "borrow-unscoped": ('view = port(1)', "AS3401"),
        "borrow-arity": ('with view = port():\n    pass', "AS3204"),
        "borrow-type": ('with view = port("1"):\n    pass', "AS3202"),
        "borrow-escape": ('with view = port(1):\n    alias = view', "AS3401"),
        "pipe-unscoped": ('pair = pipe()', "AS3401"),
        "pipe-one-owner": ('with reader = pipe():\n    pass', "AS3401"),
        "pipe-duplicate": ('with same, same = pipe():\n    pass', "AS3401"),
        "pipe-arity": ('with reader, writer = pipe(1):\n    pass', "AS3204"),
        "pipe-copy": ('with reader, writer = pipe():\n    copy = writer', "AS3401"),
        "pipe-expired": ('with reader, writer = pipe():\n    pass\nwriter.close()', "AS3205"),
        "file-two-owners": ('with reader, writer = open("x"):\n    pass', "AS3401"),
        "arity": ('with file = open():\n    pass', "AS3204"),
        "path": ('with file = open(7):\n    pass', "AS3202"),
        "nonowner": ('with file = 3:\n    pass', "AS3401"),
        "copy": ('with file = open("x"):\n    alias = file', "AS3401"),
        "box": ('with file = open("x"):\n    alias: Any = file', "AS3401"),
        "list": ('with file = open("x"):\n    alias = [file]', "AS3401"),
        "rebind": ('with file = open("x"):\n    file = 4', "AS3401"),
        "iterator": ('with file = open("x"):\n    alias = file.lines()', "AS3401"),
        "expired": ('with file = open("x"):\n    pass\nfile.close()', "AS3205"),
        "read-count": ('with file = open("x"):\n    file.read("2")', "AS3202"),
        "write-type": ('with file = open("x"):\n    file.write(1)', "AS3202"),
        "catch-owner": ('with file = open("x"):\n    try:\n        pass\n'
                        '    except IOError as file:\n        pass', "AS3401"),
        "closure": ('with file = open("x"):\n    def capture() -> i64:\n'
                    '        return file.fd()\n    capture()', "AS3401"),
    }
    declarations = {
        "field": 'class Box:\n    owner: Port\n',
        "parameter": 'def consume(owner: Port) -> None:\n    pass\n',
        "return": 'def acquire() -> Port:\n    with file = open("x"):\n        return file\n',
    }
    for name, (body, code) in cases.items():
        source = work / (name + ".as")
        source.write_text("# aether: 3\ndef main() -> None:\n" +
                          "\n".join("    " + line for line in body.splitlines()) + "\n")
        result = invoke([compiler, "check", source, "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(
            item["code"] == code for item in report["diagnostics"]), (name, report)
    for name, declaration in declarations.items():
        source = work / (name + ".as")
        source.write_text("# aether: 3\n" + declaration + "def main() -> None:\n    pass\n")
        result = invoke([compiler, "check", source, "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(
            item["code"] == "AS3401" for item in report["diagnostics"]), (name, report)


def fault_probe(runtime, work, name):
    # Mock only port.c. The real file acquisition still opens real files and
    # enforces capabilities; the test and collector retain their own syscalls.
    compiler = os.environ.get("CLANG", "clang")
    flags = ["-O0", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    obj = work / (name + ".o")
    hooks = ["-D" + symbol + "=probe_" + symbol
             for symbol in ("read", "write", "close", "calloc", "realloc", "pipe", "free")]
    result = invoke([compiler, *flags, *hooks, "-c", runtime / "port.c", "-o", obj])
    assert result.returncode == 0, result
    binary = work / name
    sources = [path for path in runtime_sources(runtime) if path.name != "port.c"]
    result = invoke([compiler, *flags, "-I", runtime, FAULTS, obj, *sources, "-o", binary])
    assert result.returncode == 0, result
    return invoke([binary, work], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1"))


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-ports-") as temporary:
        work = Path(temporary)
        diagnostics(compiler, work)
        ir = work / "ports.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "ports", optimization, (work,))
            assert result.returncode == 0 and result.stdout == "native scoped ports ok\n", result
        emit_ir(compiler, FIXTURE.with_name("borrowed.as"), ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "borrowed", optimization, (work,))
            assert result.returncode == 0 and result.stdout == (
                "borrowed stdout\nnative borrowed ports ok\n"), result
        result = fault_probe(RUNTIME, work, "faults")
        assert result.returncode == 0, result
        emit_ir(compiler, FIXTURE.with_name("pipe.as"), ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "pipe", optimization)
            assert result.returncode == 0 and result.stdout == "native pipe owners ok\n", result
        close_errors(compiler, work)
        emit_ir(compiler, FIXTURE.with_name("stats.as"), ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "stats", optimization, (work,))
            assert result.returncode == 0 and result.stdout == "native port statistics ok\n", result
    print("PASS ports: 29 diagnostics, O0/O2 file/borrow/pipe scopes, statistics, real I/O and failure paths")


def close_errors(compiler, work):
    runtime = work / "close-runtime"
    runtime.mkdir()
    copy_runtime(runtime)
    source = runtime / "port.c"
    source.write_text(source.read_text().replace(
        "int at_port_close(AtPort *port)",
        "int probe_close(int descriptor);\nint at_port_close(AtPort *port)").replace(
            "close(descriptor)", "probe_close(descriptor)"))
    shutil.copyfile(ROOT / "tests/fixtures/astyped/port-close-probe.c",
                    runtime / "port_close_probe.c")
    manifest = runtime / "sources.def"
    manifest.write_text(manifest.read_text() + "\nAT_RUNTIME_SOURCE(port_close_probe)\n")
    ir = work / "close-errors.ll"
    emit_ir(compiler, FIXTURE.with_name("close-errors.as"), ir)
    log = work / "close.log"
    previous = os.environ.get("AS_PORT_CLOSE_LOG")
    os.environ["AS_PORT_CLOSE_LOG"] = str(log)
    try:
        for optimization in ("-O0", "-O2"):
            log.write_text("")
            result = sanitized(ir, runtime, work / "close-errors", optimization, (FIXTURE,))
            assert result.returncode == 0 and result.stdout.endswith("close errors ok\n"), result
            expected = result.stdout.removesuffix("close errors ok\n")
            assert log.read_text() == expected, (optimization, log.read_text(), expected)
    finally:
        if previous is None:
            del os.environ["AS_PORT_CLOSE_LOG"]
        else:
            os.environ["AS_PORT_CLOSE_LOG"] = previous


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-port-controls-") as temporary:
        work = Path(temporary)
        copy_runtime(work)
        result = fault_probe(work, work, "positive")
        assert result.returncode == 0, result
        statistics = work / "statistics.ll"
        emit_ir(compiler, FIXTURE.with_name("stats.as"), statistics)
        result = sanitized(statistics, work, work / "statistics", "-O0", (work,))
        assert result.returncode == 0, result
        heap = work / "heap.c"
        heap_source = heap.read_text()
        anchor = "if (live_bytes + bytes >= collect_at)"
        assert heap_source.count(anchor) == 1
        heap.write_text(heap_source.replace(anchor, "if (1)"))
        result = sanitized(statistics, work, work / "statistics-gc", "-O0", (work,))
        assert result.returncode == 0, result
        stats_source = work / "port_stats.c"
        original_stats = stats_source.read_text()
        anchor = "at_gc_root(&root, &dictionary, scan_dictionary);"
        assert original_stats.count(anchor) == 1
        stats_source.write_text(original_stats.replace(anchor, "/* lost snapshot root */"))
        result = sanitized(statistics, work, work / "statistics-unrooted", "-O0", (work,))
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
        print("PASS port control: missing statistics root fails under forced-GC ASan")
        stats_source.write_text(original_stats)
        heap.write_text(heap_source)
        original = (RUNTIME / "port.c").read_text()
        mutations = {
            "lost-close-counter": ("closed_ports++;", "/* lost successful close count */"),
            "lost-close": ("int status = close(descriptor) < 0 ? io_error() : 0;", "int status = 0;"),
            "close-error": ("int status = close(descriptor) < 0 ? io_error() : 0;",
                            "close(descriptor); int status = 0;"),
            "short-write": ("*out += count;", "*out = length;"),
            "pipe-permission": ("if (!at_caps_have(AS_CAP_PROC)) {", "if (0) {"),
        }
        for name, (anchor, replacement) in mutations.items():
            assert original.count(anchor) == 1, (name, "mutation anchor changed")
            (work / "port.c").write_text(original.replace(anchor, replacement))
            result = fault_probe(work, work, name)
            assert result.returncode != 0 and "assert" in result.stderr.lower(), result
            print("PASS port control:", name, "observed assertion failure")
        anchor = "free(input);"
        assert original.count(anchor) == 2, "partial pipe allocation anchor changed"
        (work / "port.c").write_text(original.replace(anchor, "/* lost wrapper */"))
        result = fault_probe(work, work, "pipe-partial-allocation")
        assert result.returncode != 0 and "wrapper_count == 0" in result.stderr, result
        print("PASS port control: partial pipe allocation observed leaking its first wrapper")
        # A borrowed wrapper must never consume the owner's descriptor. The
        # positive fixture writes through the owner after nested cleanup.
        borrowed_ir = work / "borrowed.ll"
        emit_ir(compiler, FIXTURE.with_name("borrowed.as"), borrowed_ir)
        (work / "port.c").write_text(original)
        result = sanitized(borrowed_ir, work, work / "borrowed-ok", "-O0", (work,))
        assert result.returncode == 0, result
        anchor = "if (port->borrowed) {"
        assert original.count(anchor) == 1
        (work / "port.c").write_text(original.replace(anchor, "if (0) {"))
        result = sanitized(borrowed_ir, work, work / "borrowed-closed", "-O0", (work,))
        assert result.returncode == 1 and "IOError" in result.stderr, result
        print("PASS port control: borrowed close observed breaking the original owner")
        anchor = "if (port->borrowed && requested < capacity) {"
        assert original.count(anchor) == 1
        (work / "port.c").write_text(original.replace(anchor, "if (0) {"))
        result = sanitized(borrowed_ir, work, work / "borrowed-read-ahead", "-O0", (work,))
        assert result.returncode == 1 and "IOError" in result.stderr, result
        print("PASS port control: borrowed read-ahead observed consuming the owner's bytes")
        # Mutating emitted cleanup exercises generated return/break/exception
        # paths, independently of the runtime close implementation above.
        ir = work / "ports.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "cleanup-ok", "-O0", (work,))
        assert result.returncode == 0, result
        changed, count = re.subn(r"call i32 @at_port_release\(ptr [^\n]+\)",
                                "add i32 0, 0", ir.read_text())
        assert count > 0, "cleanup mutation anchor changed"
        ir.write_text(changed)
        result = sanitized(ir, RUNTIME, work / "cleanup-missing", "-O0", (work,))
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        print("PASS port control: missing generated cleanup observed descriptor-reuse failure")
        emit_ir(compiler, FIXTURE.with_name("pipe.as"), ir)
        result = sanitized(ir, RUNTIME, work / "pipe-cleanup-ok", "-O0")
        assert result.returncode == 0, result
        changed, count = re.subn(r"call i32 @at_port_release\(ptr [^\n]+\)",
                                "add i32 0, 0", ir.read_text())
        assert count > 0, "pipe cleanup mutation anchor changed"
        ir.write_text(changed)
        result = sanitized(ir, RUNTIME, work / "pipe-cleanup-missing", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        print("PASS port control: missing pipe cleanup observed descriptor-reuse failure")
        # A real closed read end must raise IOError rather than terminate the
        # native process under the host's default SIGPIPE disposition.
        emit_ir(compiler, FIXTURE.with_name("pipe.as"), ir)
        (work / "port.c").write_text(original)
        process = work / "process.c"
        source = process.read_text()
        anchor = "signal(SIGPIPE, SIG_IGN)"
        assert source.count(anchor) == 1, "SIGPIPE initialization anchor changed"
        process.write_text(source.replace(anchor, "signal(SIGPIPE, SIG_DFL)"))
        result = sanitized(ir, work, work / "pipe-signal-default", "-O0")
        assert result.returncode == -signal.SIGPIPE, result
        print("PASS port control: default SIGPIPE observed terminating before scoped cleanup")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
