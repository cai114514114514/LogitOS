#!/usr/bin/env python3
"""Native captured cells, lexical scope, generic environments and GC failures."""

import argparse
import json
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/closure/main.as"
EXPECTED = "native closures ok\n"


def require_output(result):
    assert result.returncode == 0 and result.stdout == EXPECTED, result


def exercise(compiler):
    cases = {
        "missing-context": ("""
            def main() -> None:
                function = lambda value: value
            """, "AS3201"),
        "wrong-result": ("""
            def main() -> None:
                function: Callable[[i64], str] = lambda value: value
            """, "AS3202"),
        "uninitialized": ("""
            def main() -> None:
                value: i64
                function: Callable[[], i64] = lambda: value
            """, "AS3206"),
        "changed-type": ("""
            def main() -> None:
                value = 1
                def change() -> None:
                    value = "text"
            """, "AS3202"),
        "borrowed-view": ("""
            def capture(values: Slice[i64]) -> Callable[[], i64]:
                return lambda: values[0]
            """, "AS3400"),
        "optional-alias": ("""
            def main() -> None:
                value: Optional[i64] = 1
                def clear() -> None:
                    value = None
                if value != None:
                    clear()
                    print(value + 1)
            """, "AS3400"),
        "no-module-export": ("""
            def outer() -> None:
                def inner() -> None:
                    pass
            def main() -> None:
                inner()
            """, "AS3205"),
        "duplicate": ("""
            def main() -> None:
                def inner() -> None:
                    pass
                def inner() -> None:
                    pass
            """, "AS3200"),
        "incomplete": ("""
            def main() -> None:
                function = lambda value:
            """, "AS3100"),
        "loop-boundary": ("""
            def main() -> None:
                for value in range(2):
                    def inner() -> None:
                        break
            """, "AS3207"),
    }
    with tempfile.TemporaryDirectory(prefix="as-closure-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                error["code"] == code for error in report["diagnostics"]), (name, report)
        ir = work / "closure.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(sanitized(ir, RUNTIME, work / "closure", optimization))
    print(f"PASS closures: {len(cases)} diagnostics, shared cells, recursive/generic capture, O0/O2 ASan")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-closure-control-") as temporary:
        work = Path(temporary)
        ir = work / "closure.ll"
        emit_ir(compiler, FIXTURE, ir)
        copy_runtime(work)
        heap = work / "heap.c"
        text = heap.read_text()
        anchor = "if (live_bytes + bytes >= collect_at)"
        assert text.count(anchor) == 1, "collector threshold anchor changed"
        heap.write_text(text.replace(anchor, "if (1)"))
        require_output(sanitized(ir, work, work / "positive", "-O0"))

        runtime = work / "closure.c"
        original = runtime.read_text()
        anchor = "at_gc_mark(environment->cells[index]);"
        assert original.count(anchor) == 1, "environment scanner anchor changed"
        runtime.write_text(original.replace(anchor, "/* omitted captured cell */"))
        result = sanitized(ir, work, work / "missing-environment-edge", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
        runtime.write_text(original)

        text, count = re.subn(
            r"^  call void @at_gc_root\(ptr %root\d+, ptr %cellroot\d+, ptr @scan\d+\)\n",
            "", ir.read_text(), flags=re.MULTILINE)
        assert count > 0, "captured local root anchor changed"
        ir.write_text(text)
        result = sanitized(ir, work, work / "missing-cell-root", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS closure controls: missing environment edges and local cell roots fail under ASan")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
