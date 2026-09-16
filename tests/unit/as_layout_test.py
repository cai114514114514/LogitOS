#!/usr/bin/env python3
"""Native ABI records: real C layout oracle, diagnostics and observed controls."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile
import sys

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_runtime import copy_runtime

sys.path.insert(0, str(ROOT / "tools"))

FIXTURE = ROOT / "tests/fixtures/astyped/native-layout/main.as"
EXPECTED = "native layouts ok\n"


def require_output(result):
    assert result.returncode == 0 and result.stdout == EXPECTED, result


def diagnostics(compiler, work):
    good = 'Packet = layout("packet", 8, [["value", 1, 2, "i"]])\n'
    cases = {
        "zero-size": ('Packet = layout("p", 0, [])', "AS3811"),
        "huge-size": ('Packet = layout("p", 65536, [])', "AS3811"),
        "expression-size": ('Packet = layout("p", 4 + 4, [])', "AS3811"),
        "negative-offset": ('Packet = layout("p", 8, [["x", -1, 1, "u"]])', "AS3811"),
        "outside": ('Packet = layout("p", 8, [["x", 7, 2, "u"]])', "AS3811"),
        "zero-width": ('Packet = layout("p", 8, [["x", 0, 0, "s"]])', "AS3811"),
        "scalar-width": ('Packet = layout("p", 8, [["x", 0, 3, "i"]])', "AS3811"),
        "pointer-width": ('Packet = layout("p", 8, [["x", 0, 4, "p"]])', "AS3811"),
        "field-kind": ('Packet = layout("p", 8, [["x", 0, 4, "f"]])', "AS3811"),
        "field-shape": ('Packet = layout("p", 8, [["x", 0, 4]])', "AS3811"),
        "field-keyword": ('Packet = layout("p", 8, [["return", 0, 4, "i"]])', "AS3811"),
        "field-punctuation": ('Packet = layout("p", 8, [["x.y", 0, 4, "i"]])', "AS3811"),
        "duplicate-field": ('Packet = layout("p", 8, [["x", 0, 1, "i"], ["x", 1, 1, "i"]])', "AS3811"),
        "duplicate-type": (good + good, "AS3811"),
        "global-conflict": (good + "Packet = 1", "AS3200"),
        "function-conflict": (good + "def Packet() -> None:\n    pass", "AS3200"),
        "runtime-factory": ('def main() -> None:\n    value = layout("p", 8, [])', "AS3811"),
        "wrong-constructor": (good + "def main() -> None:\n    Packet(1)", "AS3204"),
        "wrong-field-type": (good + "def main() -> None:\n    p = Packet()\n    p.value = 1.5", "AS3202"),
        "overflow-literal": (good + "def main() -> None:\n    p = Packet()\n    p.value = 32768", "AS3203"),
        "nominal": (good + 'Other = layout("p", 8, [["value", 1, 2, "i"]])\n'
                    "def main() -> None:\n    p: Packet = Other()", "AS3202"),
        "immutable-span": ('Packet = layout("p", 8, [["data", 0, 8, "s"]])\n'
                           "def main() -> None:\n    p = Packet()\n    p.data[0] = 1", "AS3400"),
        "text-is-not-bytes": ('Packet = layout("p", 8, [["data", 0, 8, "s"]])\n'
                              'def main() -> None:\n    p = Packet()\n    p.data = "x"', "AS3202"),
    }
    for name, (source, code) in cases.items():
        path = work / (name + ".as")
        path.write_text("# aether: 3.0\n# 中文位置\n" + source + "\n")
        result = invoke([compiler, "check", path, "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(
            item["code"] == code for item in report["diagnostics"]), (name, report)
        for item in report["diagnostics"]:
            assert item["line"] >= 3 and item["source_checksum"] == report["source_checksum"]
    report = json.loads(invoke([compiler, "check", FIXTURE, "--json"]).stdout)
    symbol = next(item for item in report["symbols"] if item["name"] == "Packet")
    assert symbol["kind"] == "type" and symbol["path"].endswith("records.as"), symbol
    return len(cases)


def abi_sources(work):
    # Enumeration comes from the ABI generator, but expected offsets and sizes
    # come from an independent C compiler using the actual kernel header. Merely
    # comparing two copies of the generator's offset table would prove nothing.
    spec = importlib.util.spec_from_file_location("layout_abi", ROOT / "tools/gen_abi.py")
    abi = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(abi)
    records = abi.read_structs()
    declarations, body = ["# aether: 3.0"], ["def main() -> None:"]
    c = ['#include "logit_abi.h"', "#include <stdio.h>", "#include <string.h>", "int main(void) {"]
    for number, (name, fields, size) in enumerate(records):
        alias = abi.as_name(name)
        declarations.append(f'{alias} = layout("{name}", {size}, [')
        declarations.extend(f'    ["{field}", {offset}, {width}, "{kind}"],'
                            for field, _, offset, width, kind in fields)
        declarations.append("])")
        body.extend([f"    record{number} = {alias}()", f"    assert len(record{number}) == {size}"])
        c.extend([f"    struct {name} record{number};",
                  f"    memset(&record{number}, 0, sizeof record{number});"])
        for index, (field, designator, _, width, kind) in enumerate(fields):
            if kind == "s":
                text = "abc"[:min(width, 3)]
                body.append(f'    record{number}.{field} = Bytes("{text}")')
                c.append(f'    memcpy(record{number}.{designator}, "{text}", {len(text)});')
            else:
                value = -(index + 1) if kind == "i" else index + 17
                body.append(f"    record{number}.{field} = {value}")
                c_value = f"(void *)(uintptr_t){value}" if kind == "p" else str(value)
                c.append(f"    record{number}.{designator} = {c_value};")
        body.extend([f"    for byte in record{number}:", "        print(byte)"])
        c.extend([f"    for (size_t i = 0; i < sizeof record{number}; i++)",
                  f'        printf("%u\\n", ((unsigned char *)&record{number})[i]);'])
    c.append("    return 0;\n}")
    source, reference = work / "all-layouts.as", work / "oracle.c"
    source.write_text("\n".join(declarations + body) + "\n")
    reference.write_text("\n".join(c) + "\n")
    return source, reference, len(records)


def guest_clock(work):
    source, _, _ = abi_sources(work)
    declarations = source.read_text().split("def main() -> None:", 1)[0]
    # The declaration is generated from the same actual header used by the C
    # oracle. The guest then tests a kernel write through addr(record), which
    # a host-only comparison of language stores cannot validate.
    source = work / "layout-clock.as"
    source.write_text(declarations + '''def main() -> None:
    clock = Time()
    alias = clock
    unsafe:
        assert syscall(SYS_GET_TIME, addr(clock)) == 0
    assert alias.year >= 2020 and alias.year <= 9999
    assert alias.month >= 1 and alias.month <= 12
    assert alias.day >= 1 and alias.day <= 31
    assert alias.hour >= 0 and alias.hour <= 23
    assert alias.minute >= 0 and alias.minute <= 59
    assert alias.second >= 0 and alias.second <= 60
    assert alias.weekday >= 0 and alias.weekday <= 6
    gc_collect()
    assert clock.year == alias.year
    print("native kernel layout ok")
''')
    return source


def c_oracle(compiler, work):
    source, reference, count = abi_sources(work)
    binary = work / "oracle"
    result = invoke([os.environ.get("CLANG", "clang"), "-I", ROOT / "include/abi",
                     reference, "-o", binary])
    assert result.returncode == 0, result.stderr
    expected = invoke([binary])
    assert expected.returncode == 0, expected
    ir = work / "all-layouts.ll"
    emit_ir(compiler, source, ir)
    for optimization in ("-O0", "-O2"):
        actual = sanitized(ir, RUNTIME, work / "all-layouts", optimization)
        assert actual.returncode == 0 and actual.stdout == expected.stdout, actual
    return count


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-layout-") as temporary:
        work = Path(temporary)
        count = diagnostics(compiler, work)
        records = c_oracle(compiler, work)
        ir = work / "layout.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(sanitized(ir, RUNTIME, work / "layout", optimization))
    print(f"PASS native layout: {count} diagnostics, {records} real C records, O0/O2 storage and GC")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-layout-controls-") as temporary:
        work = Path(temporary)
        runtime = work / "runtime"
        copy_runtime(runtime)
        heap = runtime / "heap.c"
        anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
        original = heap.read_text()
        assert original.count(anchor) == 1
        heap.write_text(original.replace(anchor, anchor + "\n    at_gc_collect();"))
        ir = work / "layout.ll"
        emit_ir(compiler, FIXTURE, ir)
        require_output(sanitized(ir, runtime, work / "positive", "-O0"))

        original = ir.read_text()
        changed, count = re.subn(r"(getelementptr i8, ptr %v\d+, i64 )1\n", r"\g<1>2\n", original)
        assert count > 0, "field offset mutation anchor changed"
        ir.write_text(changed)
        result = sanitized(ir, runtime, work / "offset", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        print("PASS layout control: wrong field offset observed AssertionError")
        ir.write_text(original)

        # Formatting must use the declared span width even though ordinary
        # Bytes reads still work. Mutate only reflection metadata, not stores.
        changed, count = re.subn(
            r"(@layoutspan\d+_\d+ = private constant %AtNativeType \{ i32 \d+, i32 8, i32 0, i32 )8,",
            r"\g<1>7,", original)
        assert count > 0, "span metadata mutation anchor changed"
        ir.write_text(changed)
        result = sanitized(ir, runtime, work / "span-metadata", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        print("PASS layout control: wrong span metadata observed AssertionError")
        ir.write_text(original)

        source = runtime / "buffer.c"
        original_buffer = source.read_text()
        anchor = "memset(record->data + offset + value->length, 0, (size_t)(width - value->length));"
        assert original_buffer.count(anchor) == 1
        source.write_text(original_buffer.replace(anchor, "/* private control: omitted padding */"))
        result = sanitized(ir, runtime, work / "padding", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        print("PASS layout control: missing zero padding observed AssertionError")
        source.write_text(original_buffer)

        # Isolate missing roots to a layout owner. Other managed types cannot
        # accidentally keep this object's storage alive for the control.
        program = work / "roots.as"
        program.write_text('''# aether: 3.0
Record = layout("record", 2, [["value", 0, 2, "u"]])
def make() -> Record:
    record = Record()
    record.value = 42
    return record
def main() -> None:
    record = make()
    gc_collect()
    assert record.value == 42
''')
        emit_ir(compiler, program, ir)
        result = sanitized(ir, runtime, work / "roots-positive", "-O0")
        assert result.returncode == 0, result
        changed, count = re.subn(r"^  call void @at_gc_root\([^\n]+\n", "", ir.read_text(), flags=re.MULTILINE)
        assert count > 0
        ir.write_text(changed)
        result = sanitized(ir, runtime, work / "roots-negative", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
        print("PASS layout control: missing record roots observed ASan failure")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    (negative_controls if args.negative_control else exercise)(args.compiler.resolve())
