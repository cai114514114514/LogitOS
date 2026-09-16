#!/usr/bin/env python3
"""Execute native ABI wrappers, with an independent word/copy oracle."""

import argparse
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_runtime import copy_runtime
from as_abi_wait_test import polling_source

sys.path.insert(0, str(ROOT / "tools"))
import gen_abi

FIXTURE = ROOT / "tests/fixtures/astyped/abi/main.as"


def metadata():
    invalid = [
        ["buf(data)"],
        ["outbuf(data)"],
        ["outbuf(data,count)"],
        ["record(data,4)"],
        ["argv(data,count)"],
        ["str(name)", "int(name)"],
        ["pack(x:0:0)"],
        ["pack(x:63:2)"],
        ["pack(x:0:8,y:7:8)"],
    ]
    for arguments in invalid:
        try:
            gen_abi.parse_args("private-metadata", 1, arguments)
        except gen_abi.Unsupported:
            continue
        raise AssertionError(f"invalid ABI declaration accepted: {arguments}")
    # A forward extent reference is legal. Rejecting it would force the source
    # signature to follow metadata traversal rather than actual syscall order.
    parsed = gen_abi.parse_args("private-metadata", 1,
                               ["outbuf(data,count)", "pack(count:0:32,flags:32:32)"])
    assert gen_abi.parameter_names(parsed) == ["data", "count", "flags"]


def diagnostics(compiler, work):
    # Studio and AI consume the same structured symbols as the CLI. Check both
    # protocol names and byte ranges against the actual ABI source snapshot.
    library_source = ROOT / "fsroot/as/lib/abi.as"
    result = invoke([compiler, "check", library_source, "--json"])
    report = json.loads(result.stdout)
    assert result.returncode == 0 and report["ok"], report
    symbols = {symbol["name"]: symbol for symbol in report["symbols"]}
    data = library_source.read_bytes()
    for name, protocol in (("io_write", "ByteStorage"), ("fd_read", "MutableByteStorage")):
        symbol = symbols[name]
        assert symbol["type_parameters"] == [{"name": "B0", "constraint": protocol}], symbol
        assert data[symbol["start"]:symbol["end"]].decode() == name, symbol

    cases = {
        "immutable-read": ('abi.fd_read(0, Bytes("x"), 1)', "AS360"),
        "immutable-random": ('abi.getrandom(Bytes("x"), 1, 0)', "AS360"),
        "wrong-record": ('abi.get_time(abi.Event())', "AS3202"),
        "text-input": ('abi.io_write(1, "x", 1)', "AS360"),
        "numeric-input": ('abi.io_write(1, 42, 1)', "AS360"),
    }
    for name, (body, code) in cases.items():
        source = work / (name + ".as")
        source.write_text("# aether: 3.0\nimport abi\ndef main() -> None:\n    " + body + "\n")
        result = invoke([compiler, "check", source, "--stdlib", ROOT / "fsroot/as/lib", "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(
            item["code"].startswith(code) for item in report["diagnostics"]), (name, report)
    for constraint in ("ByteStorage", "MutableByteStorage"):
        source = work / (constraint + ".as")
        body = "data[0] = 1" if constraint == "ByteStorage" else "pass"
        source.write_text(f"# aether: 3.0\ndef update[B: {constraint}](data: B) -> None:\n    {body}\n"
                          "def main() -> None:\n    update(Bytes(\"x\"))\n")
        result = invoke([compiler, "check", source, "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and report["diagnostics"], report


def transport_source(work):
    calls = []
    for entry in gen_abi.parse_calls():
        if entry[0] == "call":
            calls.append(entry[1:])
        else:
            _, name, start, poll, arguments, _, _ = entry
            calls.extend([(name + "_start", start, arguments), (name + "_poll", poll, [])])
    source = ["# aether: 3.0", "import abi", "def main() -> None:"]
    expected = []
    for number, (name, symbol, arguments) in enumerate(calls):
        values, words = [], []
        for index, (kind, body) in enumerate(arguments):
            variable = f"arg_{number}_{index}"
            if kind == "str":
                source.append(f'    {variable} = ("prefix文本" + "suffix").slice(6, 12)')
                values.append(variable)
                words.append((2, 0))
            elif kind in ("inbuf", "outbuf", "argv", "record"):
                expression = f"abi.{body[1]}()" if kind == "record" else "buffer(128)"
                if kind == "inbuf":
                    expression = f"Bytes({expression})"
                source.append(f"    {variable} = {expression}")
                values.append(variable)
                words.append((1, 0))
            elif kind == "int":
                values.append("3")
                words.append((0, 3))
            elif kind == "lit":
                words.append((0, int(body, 0) & ((1 << 64) - 1)))
            else:
                values.extend("-1" for _ in body)
                # Independent integer arithmetic, not the language generator's
                # expression renderer. This includes bit 63 and masks negatives.
                packed = sum(((1 << width) - 1) << shift for _, shift, width in body)
                words.append((0, packed))
        words.extend([(0, 0)] * (3 - len(words)))
        expected.append((symbol, words))
        source.append(f"    assert abi.{name}({', '.join(values)}) == -123")
    source.append('    print("native ABI transport ok")')
    path = work / "transport.as"
    path.write_text("\n".join(source) + "\n")

    c = ['#include "abi/logit_abi.h"', "#include <assert.h>", "#include <stdint.h>",
         "#include <string.h>",
         "struct word { int kind; uint64_t value; };",
         "struct call { int64_t number; struct word args[3]; };",
         "static const struct call expected[] = {"]
    for symbol, words in expected:
        args = ", ".join(f"{{{kind}, UINT64_C({value})}}" for kind, value in words)
        c.append(f"    {{{symbol}, {{{args}}}}},")
    c.extend(["};", "static unsigned seen;",
              "int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c) {",
              "    assert(seen < sizeof expected / sizeof *expected);",
              "    const struct call *call = &expected[seen++];",
              "    assert(number == call->number);",
              "    uint64_t actual[] = {a, b, c};",
              "    for (unsigned i = 0; i < 3; i++) {",
              "        if (call->args[i].kind == 0) assert(actual[i] == call->args[i].value);",
              "        else if (call->args[i].kind == 1) {",
              "            assert(actual[i] && *(const unsigned char *)(uintptr_t)actual[i] == 0);",
              '        } else assert(!strcmp((const char *)(uintptr_t)actual[i], "文本"));',
              "    }", "    return -123;", "}",
              "__attribute__((destructor)) static void check_count(void) {",
              "    assert(seen == sizeof expected / sizeof *expected);", "}"])
    return path, "\n".join(c) + "\n", len(calls)


def probe_runtime(work, probe):
    runtime = work / "runtime"
    copy_runtime(runtime)
    shutil.copytree(ROOT / "include/abi", runtime / "abi")
    path = runtime / "system.c"
    original = path.read_text()
    anchor = "return AT_E_RUNTIME;"
    assert original.count(anchor) == 1
    path.write_text(original.replace(anchor,
        "extern int64_t abi_probe(int64_t, uint64_t, uint64_t, uint64_t);\n"
        "    *out = abi_probe(number, a, b, c);\n    return 0;"))
    (runtime / "abi_probe.c").write_text(probe)
    manifest = runtime / "sources.def"
    manifest.write_text(manifest.read_text() + "\nAT_RUNTIME_SOURCE(abi_probe)\n")
    return runtime


def exercise(compiler):
    metadata()
    with tempfile.TemporaryDirectory(prefix="as-native-abi-") as temporary:
        work = Path(temporary)
        diagnostics(compiler, work)
        source, probe, count = transport_source(work)
        runtime = probe_runtime(work, probe)
        ir = work / "transport.ll"
        emit_ir(compiler, source, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, runtime, work / "transport", optimization)
            assert result.returncode == 0 and result.stdout == "native ABI transport ok\n", result
        for name, source, expected in (
            ("guards", FIXTURE, "native ABI argument guards ok\n"),
            ("storage", ROOT / "tests/fixtures/astyped/storage/main.as", "native storage protocols ok\n"),
        ):
            emit_ir(compiler, source, ir)
            for optimization in ("-O0", "-O2"):
                result = sanitized(ir, RUNTIME, work / name, optimization)
                assert result.returncode == 0 and result.stdout == expected, result
        polling = work / "polling"
        polling.mkdir()
        source, probe, waits = polling_source(polling)
        runtime = probe_runtime(polling, probe)
        emit_ir(compiler, source, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, runtime, polling / "program", optimization)
            assert result.returncode == 0 and result.stdout == "native ABI polling ok\n", result
    print(f"PASS native ABI: {count} wrapper transports, {waits} polling cases, "
          "storage constraints, bounds and C strings, O0/O2")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-abi-controls-") as temporary:
        work = Path(temporary)
        source, probe, _ = transport_source(work)
        runtime = probe_runtime(work, probe)
        heap = runtime / "heap.c"
        anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
        original = heap.read_text()
        assert original.count(anchor) == 1
        heap.write_text(original.replace(anchor, anchor + "\n    at_gc_collect();"))
        ir = work / "transport.ll"
        emit_ir(compiler, source, ir)
        result = sanitized(ir, runtime, work / "positive", "-O0")
        assert result.returncode == 0, result

        library = work / "lib"
        shutil.copytree(ROOT / "fsroot/as/lib", library)
        abi = library / "abi.as"
        original = abi.read_text()
        mutations = [
            ("wrapping_shl((timeout & 0xFFFFFFFF), 32)", "((timeout & 0xFFFFFFFF) << 32)", "packed-sign"),
            ('return Bytes(value + chr(0))', 'return Bytes(value + "suffix" + chr(0))', "c-string-view"),
        ]
        for anchor, replacement, label in mutations:
            assert original.count(anchor) == 1, label
            abi.write_text(original.replace(anchor, replacement))
            result = invoke([compiler, "build", source, "--stdlib", library, "--emit-llvm", "-o", ir])
            assert result.returncode == 0, result
            result = sanitized(ir, runtime, work / label, "-O0")
            assert result.returncode != 0, (label, result)
            assert "OverflowError" in result.stderr or "assert" in result.stderr.lower(), result
            print("PASS ABI control:", label, "observed failing")
        abi.write_text(original)

        # Bound checking must fail before reaching the platform. The unmodified
        # host runtime rejects raw LogitOS calls, making this control safe while
        # still distinguishing ValueError from an accidentally attempted call.
        anchor = "if count < 0 or count > len(data):"
        assert original.count(anchor) == 1
        abi.write_text(original.replace(anchor, "if false:"))
        result = invoke([compiler, "build", FIXTURE, "--stdlib", library, "--emit-llvm", "-o", ir])
        assert result.returncode == 0, result
        result = sanitized(ir, RUNTIME, work / "missing-bound", "-O0")
        assert result.returncode != 0 and "RuntimeError" in result.stderr, result
        print("PASS ABI control: missing-bound observed failing")

        polling = work / "polling"
        polling.mkdir()
        source, probe, _ = polling_source(polling)
        runtime = probe_runtime(polling, probe)
        anchor = "if now - start >= duration:"
        assert original.count(anchor) == 1
        abi.write_text(original.replace(anchor, "if now - start > duration:"))
        result = invoke([compiler, "build", source, "--stdlib", library, "--emit-llvm", "-o", ir])
        assert result.returncode == 0, result
        result = sanitized(ir, runtime, polling / "late-timeout", "-O0")
        assert result.returncode != 0 and "assert" in result.stderr.lower(), result
        print("PASS ABI control: late-timeout observed failing")
        abi.write_text(original)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    (negative_controls if args.negative_control else exercise)(args.compiler.resolve())
