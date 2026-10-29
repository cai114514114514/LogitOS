#!/usr/bin/env python3
"""A3 pipe snapshots: multiple unsaved files, real execution, strict framing."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from as_managed_test import ROOT


def encode(entries):
    result = bytearray(b"AETHER-SNAPSHOT-1\n" + str(len(entries)).encode() + b"\n")
    for path, source in entries:
        path = str(path).encode()
        source = source.encode() if isinstance(source, str) else source
        result.extend(f"{len(path)}\n{len(source)}\n".encode())
        result.extend(path)
        result.extend(source)
    return bytes(result)


def invoke(compiler, command, source, payload, *options):
    return subprocess.run([str(compiler), command, str(source), "--snapshot-stdin", *options],
                          input=payload, capture_output=True, timeout=120)


def exercise(compiler, work):
    directory = work / "中文工程"
    directory.mkdir()
    entry, module = directory / "main.as", directory / "m.as"
    disk_entry = "# aether: 3.0\ndef main() -> None:\n    print(999)\n"
    disk_module = "# aether: 3.0\nvalue: str = \"disk\"\n"
    entry.write_text(disk_entry)
    module.write_text(disk_module)
    source = ("# aether: 3.0\nfrom m import value\n"
              "def main() -> None:\n    print(value)\n"
              "def test_snapshot() -> None:\n    assert value == 37\n")
    unsaved_module = "# aether: 3.0\nvalue: i64 = 37\n"
    payload = encode([(entry, source), (module, unsaved_module)])
    for command in ("check", "run", "test"):
        result = invoke(compiler, command, entry, payload, "--json")
        assert result.returncode == 0, (command, result)
        report = json.loads(result.stdout)
        snapshot = report if command == "check" else report["snapshot"]
        assert snapshot["ok"] and snapshot["language"] == 3, snapshot
        assert {s["path"] for s in snapshot["sources"]} == {str(entry), str(module)}, snapshot
        if command == "run":
            assert report["output"] == "37\n", report
    ir = work / "snapshot.ll"
    result = invoke(compiler, "build", entry, payload, "--emit-llvm", "-o", str(ir))
    assert result.returncode == 0 and "define" in ir.read_text(), result
    assert entry.read_text() == disk_entry and module.read_text() == disk_module

    bad_module = '# aether: 3.0\nvalue: i64 = "中文"\n'
    result = invoke(compiler, "check", entry, encode([(entry, source), (module, bad_module)]), "--json")
    report = json.loads(result.stdout)
    assert result.returncode == 1 and len(report["diagnostics"]) == 1, report
    problem = report["diagnostics"][0]
    assert problem["path"] == str(module) and problem["code"] == "AS3202", problem
    assert bad_module.encode()[problem["start"]:problem["end"]].decode().strip('"') == "中文", problem

    # No missing tail, oversized length or extra entry may quietly cause the
    # checker to read stale disk content for an omitted unsaved module.
    malformed = [
        b"", b"WRONG\n", b"AETHER-SNAPSHOT-1\n0\n", b"AETHER-SNAPSHOT-1\n65\n",
        b"AETHER-SNAPSHOT-1\n1\n512\n0\n",
        b"AETHER-SNAPSHOT-1\n1\n1\n1048577\n",
        b"AETHER-SNAPSHOT-1\n-1\n", payload + b"extra", payload[:-1],
        encode([(entry, source), (entry, source)]), encode([(entry, b"bad\0text")]),
        encode([(str(entry) + "\0suffix", source)]),
    ]
    for broken in malformed:
        result = invoke(compiler, "check", entry, broken, "--json")
        assert result.returncode == 2 and not result.stdout, result
    print("PASS source snapshots: check/build/run/test, unsaved imports, UTF-8 spans, 12 framing refusals")


def parser_sanitizer(work):
    # The owning parser also runs under ASan/UBSan. Every partial prefix must
    # be releasable, including allocations made just before a short read.
    harness = work / "reader.c"
    harness.write_text('''#include "include/snapshot.h"
int main(void)
{
    AsSourceSnapshot snapshot = {0};
    const char *error = as_snapshot_read(stdin, &snapshot);
    as_snapshot_free(&snapshot);
    return error ? 2 : 0;
}
''')
    binary = work / "reader"
    subprocess.run([os.environ.get("CC", "clang"), "-O1", "-g", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "c/apps/as"), str(harness),
                    str(ROOT / "c/apps/as/common/snapshot.c"), "-o", str(binary)], check=True)
    payload = encode([("/main.as", "# aether: 3.0\n"), ("/m.as", "# 中文\n")])
    for size in range(len(payload) + 1):
        result = subprocess.run([str(binary)], input=payload[:size], capture_output=True, timeout=10)
        assert result.returncode == (0 if size == len(payload) else 2) and not result.stderr, result
    print("PASS snapshot ownership: every truncated byte prefix rejected under ASan/UBSan")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-snapshot-") as temporary:
        work = Path(temporary).resolve()
        exercise(args.compiler.resolve(), work)
        parser_sanitizer(work)
