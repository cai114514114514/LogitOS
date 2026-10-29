#!/usr/bin/env python3
"""Real multi-module binary files and strict native UTF-8 decoding."""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime, runtime_sources

FIXTURES = ROOT / "tests/fixtures/astyped"
DECODE = FIXTURES / "decode/main.as"
BINARY = FIXTURES / "binary/main.as"
CREATE = FIXTURES / "binary/create.as"
DECODE_OUTPUT = "native UTF-8 decode ok\n"


def binary_samples():
    # This encoder is independent of the A3 reader's shifts and checks. The
    # guest and host install these same artifacts, not separate canned headers.
    payload = bytes(range(256)) * 16
    return {
        "valid": (struct.pack("<II", 3, len(payload)) + payload, 0, "header 3 4096\n"),
        "empty": (struct.pack("<II", 3, 0), 0, "header 3 0\n"),
        "truncated": (b"\x03\x00", 1, "ValueError"),
        "length": (struct.pack("<II", 3, 1), 1, "ValueError"),
        "version": (struct.pack("<II", 4, 0), 1, "ValueError"),
        "byteorder": (struct.pack(">II", 3, 0), 1, "ValueError"),
    }


def exercise(compiler):
    cases = {
        "decode-arguments": ('Bytes("a").decode("utf-8")', "AS3204"),
        "decode-receiver": ('buffer(2).decode()', "AS3202"),
        "decode-result": ('value: Bytes = Bytes("a").decode()', "AS3202"),
    }
    with tempfile.TemporaryDirectory(prefix="as-binary-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\ndef main() -> None:\n    " + body + "\n")
            report = invoke([compiler, "check", source, "--json"])
            diagnostics = json.loads(report.stdout)["diagnostics"]
            assert report.returncode == 1 and any(item["code"] == code for item in diagnostics), diagnostics
        ir = work / "decode.ll"
        emit_ir(compiler, DECODE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "decode", optimization)
            assert result.returncode == 0 and result.stdout == DECODE_OUTPUT, result

        # Exhaust every scalar, including noncharacters, and each possible
        # truncation of its encoding. Runtime validation must reject each cut.
        binary = work / "utf8"
        result = invoke([os.environ.get("CLANG", "clang"), "-O2", "-g",
                         "-fsanitize=address,undefined", "-I", RUNTIME,
                         FIXTURES / "utf8-runtime.c", *runtime_sources(), "-o", binary])
        assert result.returncode == 0, result
        result = invoke([binary], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1"))
        assert result.returncode == 0, result

        emit_ir(compiler, BINARY, ir)
        for optimization in ("-O0", "-O2"):
            executable = work / "binary"
            for index, (name, (data, status, output)) in enumerate(binary_samples().items()):
                path = work / (name + ".bin")
                path.write_bytes(data)
                if index == 0:
                    result = sanitized(ir, RUNTIME, executable, optimization, (str(path),))
                else:
                    result = invoke([executable, path], env=dict(
                        os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1"))
                assert result.returncode == status, result
                if status:
                    assert output in result.stderr and "reader.as:" in result.stderr, result
                else:
                    assert result.stdout == output, result
            result = invoke([executable])
            assert result.returncode == 2 and result.stdout == "usage: binary FILE\n", result
            result = invoke([executable, work / "missing.bin"])
            assert result.returncode == 1 and "IOError" in result.stderr, result
        emit_ir(compiler, CREATE, ir)
        for optimization in ("-O0", "-O2"):
            path = work / "generated.bin"
            result = sanitized(ir, RUNTIME, work / "create", optimization, (str(path),))
            assert result.returncode == 0 and result.stdout == "wrote 4104 bytes\n", result
            assert path.read_bytes() == binary_samples()["valid"][0]
            result = invoke([work / "binary", path])
            assert result.returncode == 0 and result.stdout == "header 3 4096\n", result
    print("PASS binary/UTF-8: 3 diagnostics, every scalar/truncation, 6 file formats, GC and O0/O2")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-decode-controls-") as temporary:
        work = Path(temporary)
        copy_runtime(work)
        ir = work / "decode.ll"
        emit_ir(compiler, DECODE, ir)
        result = sanitized(ir, work, work / "positive", "-O0")
        assert result.returncode == 0 and result.stdout == DECODE_OUTPUT, result
        original = (RUNTIME / "bytes.c").read_text()
        for name, anchor, replacement, failure in (
            ("overlong", "point < minimum", "0", "AssertionError"),
            ("truncated", "bytes->length - offset < trailing", "0", "heap-buffer-overflow"),
        ):
            assert original.count(anchor) == 1
            (work / "bytes.c").write_text(original.replace(anchor, replacement))
            result = sanitized(ir, work, work / name, "-O0")
            assert result.returncode != 0 and failure in result.stderr, result
        (work / "bytes.c").write_text(original)

        source = work / "roots.as"
        source.write_text('''# aether: 3.0
def text() -> str:
    return Bytes("中文").decode()
def main() -> None:
    value = text()
    gc_collect()
    assert value == "中文"
''')
        emit_ir(compiler, source, ir)
        result = sanitized(ir, work, work / "roots-positive", "-O0")
        assert result.returncode == 0, result
        changed, count = re.subn(r"^  call void @at_gc_root\([^\n]+\n", "", ir.read_text(),
                                 flags=re.MULTILINE)
        assert count > 0
        ir.write_text(changed)
        result = sanitized(ir, work, work / "roots-negative", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS UTF-8 controls: overlong acceptance, truncated reads and lost decoded owner fail")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
