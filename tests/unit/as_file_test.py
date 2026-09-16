#!/usr/bin/env python3
"""Native whole-file I/O, real scope enforcement and deterministic cleanup."""
import argparse
import json
import os
from pathlib import Path
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime, runtime_sources

FIXTURE = ROOT / "tests/fixtures/astyped/files/main.as"
FAULTS = ROOT / "tests/fixtures/astyped/file-runtime.c"
SCOPES = ROOT / "tests/fixtures/astyped/file-scope.c"


def fault_probe(runtime, work, name):
    """Only the file translation unit gets mocked syscalls, never the heap or
    the test's assertion/output machinery. This keeps failures attributable."""
    compiler = os.environ.get("CLANG", "clang")
    flags = ["-O0", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    obj = work / (name + ".o")
    hooks = ["-D" + symbol + "=probe_" + symbol
             for symbol in ("open", "openat", "read", "write", "close", "realloc")]
    result = invoke([compiler, *flags, *hooks, "-c", runtime / "file.c", "-o", obj])
    assert result.returncode == 0, result
    binary = work / name
    sources = [p for p in runtime_sources(runtime) if p.name != "file.c"]
    result = invoke([compiler, *flags, "-I", runtime, FAULTS, obj, *sources, "-o", binary])
    assert result.returncode == 0, result
    return invoke([binary], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1"))


def scope_probe(runtime, work, name):
    root = (work / name / "state").resolve()
    outside = root.with_name("state-outside")
    (root / "child").mkdir(parents=True)
    outside.mkdir()
    (root / "child/data").write_bytes(b"yes")
    (outside / "data").write_bytes(b"yes")
    (root / "link").symlink_to(outside / "data")
    (root / "directory-link").symlink_to(outside, target_is_directory=True)
    compiler = os.environ.get("CLANG", "clang")
    binary = work / (name + "-probe")
    result = invoke([compiler, "-O2", "-g", "-fsanitize=address,undefined", "-I", runtime,
                     SCOPES, *runtime_sources(runtime), "-o", binary])
    assert result.returncode == 0, result
    result = invoke([binary, root], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1"))
    assert (outside / "data").read_bytes() == b"yes", "write escaped the private grant"
    return result


def exercise(compiler):
    cases = {
        "read-arity": ("file_read()", "AS3204"),
        "path": ("file_read(1)", "AS3202"),
        "write-arity": ('file_write("x")', "AS3204"),
        "text": ('file_write("x", "data")', "AS3202"),
        "mutable": ('file_write("x", buffer(2))', "AS3202"),
        "result": ('value: str = file_read("x")', "AS3202"),
    }
    with tempfile.TemporaryDirectory(prefix="as-files-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\ndef main() -> None:\n    " + body + "\n")
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), (name, report)
        (work / "fixture").write_bytes(b"native guest acceptance\n")
        ir = work / "files.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "files", optimization, (str(work),))
            assert result.returncode == 0 and result.stdout == "native files ok\n", result
        assert fault_probe(RUNTIME, work, "faults").returncode == 0
        assert scope_probe(RUNTIME, work, "scopes").returncode == 0
    print("PASS files: 6 diagnostics, real O0/O2 I/O, partial transfers, EINTR, failures, closes and scopes")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-file-controls-") as temporary:
        work = Path(temporary)
        copy_runtime(work)
        result = fault_probe(work, work, "positive")
        assert result.returncode == 0, result
        original = (RUNTIME / "file.c").read_text()
        mutations = {
            "short-write": ("written += count;", "written = data->length;"),
            "lost-close": ("if (close(descriptor) < 0 && !status)", "if (0 && !status)"),
            "close-error": ("if (close(descriptor) < 0 && !status)",
                            "if (close(descriptor) < 0 && 0)"),
        }
        for name, (anchor, replacement) in mutations.items():
            assert anchor in original, (name, "mutation anchor changed")
            (work / "file.c").write_text(original.replace(anchor, replacement))
            result = fault_probe(work, work, name)
            assert result.returncode != 0 and "assert" in result.stderr.lower(), result
        (work / "file.c").write_text(original)
        result = scope_probe(work, work, "scope-positive")
        assert result.returncode == 0, result
        # Drop only the permission-bit check in the private runtime. Real
        # directory fixtures must catch an unauthorized write before success.
        anchor = "if (!at_caps_have(permission))"
        assert original.count(anchor) == 1
        (work / "file.c").write_text(original.replace(anchor, "if (0)"))
        result = scope_probe(work, work, "scope-negative")
        assert result.returncode != 0 and "assert" in result.stderr.lower(), result
        anchor = "wanted | O_NOFOLLOW | O_CLOEXEC"
        assert original.count(anchor) == 1
        (work / "file.c").write_text(original.replace(anchor, "wanted | O_CLOEXEC"))
        result = scope_probe(work, work, "symlink-negative")
        assert result.returncode != 0 and "assert" in result.stderr.lower(), result
    print("PASS file controls: partial write, missing close, swallowed close error, lost authority and symlink traversal fail")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
