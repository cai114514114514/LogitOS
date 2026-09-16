#!/usr/bin/env python3
"""Native inheritance: stable layouts, dispatch, constructor safety and GC."""

import argparse
import json
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/inheritance/main.as"
EXPECTED = "native inheritance ok\n"


def require_output(result):
    assert result.returncode == 0 and result.stdout == EXPECTED, result


def exercise(compiler):
    cases = {
        "shadowed-self": ("""
            class Base:
                def read(self) -> i64:
                    return 1
            class Child(Base):
                def callback(self) -> Callable[[i64], i64]:
                    def inner(self: i64) -> i64:
                        return super.read()
                    return inner
            """, "AS3211"),
        "captured-self-rebind": ("""
            class Box:
                def change(self, other: Box) -> None:
                    def inner() -> None:
                        self = other
                    inner()
            """, "AS3210"),
        "captured-self-unpack": ("""
            class Box:
                def change(self, other: Box) -> None:
                    def inner() -> None:
                        self, value = other, 1
                    inner()
            """, "AS3210"),
        "self-loop": ("""
            class Box:
                def change(self, other: Box) -> None:
                    for self in [other]:
                        pass
            """, "AS3210"),
        "grandparent-init-fields": ("""
            class Grandparent:
                def init(self) -> None:
                    pass
            class Parent(Grandparent):
                value: i64
            class Child(Parent):
                def init(self) -> None:
                    super.init()
            """, "AS3210"),
        "base-kind": ("""
            class Bad(i64):
                pass
            """, "AS3211"),
        "self-base": ("""
            class Bad(Bad):
                pass
            """, "AS3211"),
        "return-override": ("""
            class Base:
                def get(self) -> i64:
                    return 1
            class Child(Base):
                def get(self) -> str:
                    return "bad"
            """, "AS3211"),
        "parameter-override": ("""
            class Base:
                def read(self, value: i64) -> i64:
                    return value
            class Child(Base):
                def read(self, value: str) -> i64:
                    return 0
            """, "AS3211"),
        "field-override": ("""
            class Base:
                value: i64
            class Child(Base):
                value: str
            """, "AS3200"),
        "field-method": ("""
            class Base:
                def read(self) -> i64:
                    return 1
            class Child(Base):
                read: i64
            """, "AS3200"),
        "extra-fields": ("""
            class Base:
                def init(self) -> None:
                    pass
            class Child(Base):
                value: i64
            def main() -> None:
                Child()
            """, "AS3210"),
        "parent-init-failure": ("""
            class Base:
                value: i64
                def init(self) -> None:
                    self.value = parse_int("bad")
            class Child(Base):
                def init(self) -> None:
                    try:
                        super.init()
                    except ConversionError:
                        pass
            """, "AS3210"),
        "init-callback": ("""
            class Base:
                def init(self) -> None:
                    pass
            class Child(Base):
                def init(self) -> None:
                    callback = super.init
            """, "AS3211"),
        "super-outside": ("""
            def main() -> None:
                super.get()
            """, "AS3211"),
        "super-field": ("""
            class Base:
                value: i64
            class Child(Base):
                def read(self) -> i64:
                    return super.value
            """, "AS3211"),
        "downcast": ("""
            class Base:
                pass
            class Child(Base):
                pass
            def wrong(value: Base) -> Child:
                return value
            """, "AS3202"),
        "container-covariance": ("""
            class Base:
                pass
            class Child(Base):
                pass
            def wrong(values: List[Child]) -> List[Base]:
                return values
            """, "AS3202"),
        "missing-base": ("""
            class Bad(Missing):
                pass
            """, "AS3200"),
    }
    with tempfile.TemporaryDirectory(prefix="as-inheritance-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == code for item in report["diagnostics"]), (name, report)
        ir = work / "inheritance.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(sanitized(ir, RUNTIME, work / "inheritance", optimization))
    print(f"PASS native inheritance: {len(cases)} diagnostics, O0/O2 dispatch, super, captures and GC")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-inheritance-control-") as temporary:
        work = Path(temporary)
        ir = work / "inheritance.ll"
        emit_ir(compiler, FIXTURE, ir)
        source = ir.read_text()
        copy_runtime(work)
        heap = work / "heap.c"
        text = heap.read_text()
        anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
        assert text.count(anchor) == 1, "allocation mutation anchor changed"
        heap.write_text(text.replace(anchor, anchor + "\n    at_gc_collect();"))
        require_output(sanitized(ir, work, work / "positive", "-O0"))

        # Removing the concrete object's initialization check must let both
        # deliberately incomplete objects escape and fail the source oracle.
        runtime = work / "object.c"
        original = runtime.read_text()
        anchor = "return header->required == header->initialized;"
        assert original.count(anchor) == 1, "readiness mutation anchor changed"
        runtime.write_text(original.replace(anchor, "return 1;"))
        result = sanitized(ir, work, work / "unready", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        runtime.write_text(original)

        # Replace virtual selection with the statically visible declaration.
        # Parent calls would now silently bypass the child's override.
        pattern = (r"  (%v\d+) = load ptr, ptr (%v\d+)\n"
                   r"  (%v\d+) = getelementptr ptr, ptr \1, i32 (\d+)\n"
                   r"  (%v\d+) = load ptr, ptr \3\n")
        def static_target(match):
            return f"  {match[5]} = select i1 true, ptr @bound{match[4]}, ptr null\n"
        mutated, count = re.subn(pattern, static_target, source)
        assert count > 0, "virtual selection mutation anchor changed"
        ir.write_text(mutated)
        result = sanitized(ir, work, work / "static", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result

        # Parent-typed references still retain the concrete child's scanner.
        # Removing that scanner must produce a real invalid managed read.
        pattern = r"(define internal void @objectscan\d+\(ptr %object\) \{\nentry:\n).*?(  ret void\n\})"
        mutated, count = re.subn(pattern, r"\1\2", source, flags=re.DOTALL)
        assert count > 0, "class scanner mutation anchor changed"
        ir.write_text(mutated)
        result = sanitized(ir, work, work / "unscanned", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS inheritance controls: missing readiness, static dispatch and unscanned fields fail")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
