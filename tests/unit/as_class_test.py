#!/usr/bin/env python3
"""Native class layout, bound receiver ownership and constructor data flow."""

import argparse
import json
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/class/main.as"


def exercise(compiler):
    cases = {
        "uninitialized": ("""
            class Box:
                value: i64
                def init(self) -> None:
                    pass
            """, "AS3210"),
        "read-before-store": ("""
            class Box:
                value: i64
                def init(self) -> None:
                    self.value = self.value + 1
            """, "AS3210"),
        "compound-before-store": ("""
            class Box:
                value: i64
                def init(self) -> None:
                    self.value += 1
            """, "AS3210"),
        "partial-branch": ("""
            class Box:
                value: i64
                def init(self, flag: bool) -> None:
                    if flag:
                        self.value = 1
            """, "AS3210"),
        "loop-only": ("""
            class Box:
                value: i64
                def init(self) -> None:
                    for i in range(3):
                        self.value = i
            """, "AS3210"),
        "early-return": ("""
            class Box:
                value: i64
                def init(self, flag: bool) -> None:
                    if flag:
                        return
                    self.value = 1
            """, "AS3210"),
        "escape": ("""
            class Box:
                value: i64
                def init(self) -> None:
                    value = Any(self)
                    self.value = 1
            """, "AS3210"),
        "method-escape": ("""
            class Box:
                value: i64
                def init(self) -> None:
                    callback = self.get
                    self.value = 1
                def get(self) -> i64:
                    return self.value
            """, "AS3210"),
        "expression-escape": ("""
            class Box:
                value: i64
                def init(self) -> None:
                    self.value = identity(self).value
            def identity(value: Box) -> Box:
                return value
            """, "AS3210"),
        "handler-entry": ("""
            class Box:
                value: i64
                def init(self) -> None:
                    try:
                        self.value = parse_int("bad")
                    except ConversionError:
                        pass
            """, "AS3210"),
        "self-rebind": ("""
            class Box:
                value: i64
                def set(self, other: Box) -> None:
                    self = other
            """, "AS3210"),
        "extra-field": ("""
            class Box:
                value: i64
            def write(value: Box) -> None:
                value.extra = 2
            """, "AS3205"),
        "method-replace": ("""
            class Box:
                def method(self) -> None:
                    pass
            def write(value: Box) -> None:
                value.method = value.method
            """, "AS3202"),
        "field-method-collision": ("""
            class Box:
                get: i64
                def get(self) -> i64:
                    return 1
            """, "AS3200"),
        "borrow-field": ("""
            class Box:
                value: Optional[Slice[i64]]
            """, "AS3400"),
        "private-method": ("""
            class Box:
                def _get(self) -> i64:
                    return 1
            def read(value: Box) -> i64:
                return value._get()
            """, "AS3300"),
        "constructor-result": ("""
            class Box:
                def init(self) -> i64:
                    return 1
            """, "AS3210"),
        "constructor-arity": ("""
            class Box:
                value: i64
            def main() -> None:
                Box()
            """, "AS3204"),
    }
    with tempfile.TemporaryDirectory(prefix="as-class-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                diagnostic["code"] == code for diagnostic in report["diagnostics"]), (name, report)
        ir = work / "class.ll"
        emit_ir(compiler, FIXTURE, ir)
        arguments = [compiler, "check", FIXTURE, "--stdlib", ROOT / "fsroot/as/lib"]
        plain = invoke(arguments)
        report = json.loads(invoke([*arguments, "--json"]).stdout)
        assert plain.returncode == 0 and report["ok"], (plain, report)
        for source in report["sources"]:
            assert plain.stdout.count(source["path"] + ": check passed\n") == 1, plain.stdout
        methods = [symbol for symbol in report["symbols"] if symbol["kind"] == "method"]
        assert any(symbol["name"] == "add" and symbol["owner"] == "Counter" for symbol in methods)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "class", optimization)
            assert result.returncode == 0 and result.stdout == "native classes ok\n", result
    print(f"PASS native class: {len(cases)} diagnostics, O0/O2 constructors, aliases, methods, cycles and GC")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-class-control-") as temporary:
        work = Path(temporary)
        ir = work / "class.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "class-ok", "-O0")
        assert result.returncode == 0, result
        source = ir.read_text()
        pattern = r"(define internal void @objectscan\d+\(ptr %object\) \{\nentry:\n).*?(  ret void\n\})"
        mutated, count = re.subn(pattern, r"\1\2", source, flags=re.DOTALL)
        assert count >= 3, "class payload scanner mutation anchor changed"
        ir.write_text(mutated)
        result = sanitized(ir, RUNTIME, work / "class-broken", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
        # A bound method is the only surviving reference to make_callback's
        # receiver. Forgetting its environment root must free that receiver.
        pattern = r"(  %environment = getelementptr \{ ptr, ptr \}[^\n]+\n  %pointer = load ptr, ptr %environment\n)  call void @at_gc_mark\(ptr %pointer\)\n"
        mutated, count = re.subn(pattern, r"\1", source)
        assert count > 0, "bound method environment scanner mutation anchor changed"
        ir.write_text(mutated)
        result = sanitized(ir, RUNTIME, work / "bound-broken", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS negative controls: unscanned class fields and bound receivers fail under ASan")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())


if __name__ == "__main__":
    main()
