#!/usr/bin/env python3
"""Optional flow facts, native payloads and GC ownership with failing controls."""

import argparse
import json
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/astyped/optional/main.as"


def exercise(compiler):
    cases = {
        "unguarded-iterable": ("""
            def read(x: Optional[List[str]]) -> None:
                for item in x:
                    print(item)
            """, "AS3202"),
        "for-body-backedge": ("""
            def read(x: Optional[List[str]]) -> None:
                assert x is not None
                for item in x:
                    print(len(x))
                    x = None
            """, "AS3202"),
        "unguarded": ("def read(x: Optional[str]) -> i64:\n    return len(x)\n", "AS3202"),
        "reassigned": ("""
            def read(x: Optional[str]) -> i64:
                assert x != None
                x = None
                return len(x)
            """, "AS3202"),
        "partial-branch": ("""
            def read(x: Optional[str], flag: bool) -> i64:
                if flag:
                    assert x != None
                return len(x)
            """, "AS3202"),
        "loop-backedge": ("""
            def read(x: Optional[str], flag: bool) -> None:
                assert x != None
                while flag:
                    print(len(x))
                    x = None
            """, "AS3202"),
        "handler-assignment": ("""
            def read(x: Optional[str]) -> None:
                assert x != None
                try:
                    x = None
                    raise ValueError("stop")
                except ValueError:
                    print(len(x))
            """, "AS3202"),
        "mutable-field": ("""
            struct Box:
                text: Optional[str]
            def read(x: Box) -> i64:
                if x.text != None:
                    return len(x.text)
                return 0
            """, "AS3202"),
        "mutable-global": ("""
            text: Optional[str] = "x"
            def read() -> i64:
                if text != None:
                    return len(text)
                return 0
            """, "AS3202"),
        "wrong-branch": ("""
            def read(x: Optional[str]) -> i64:
                if x == None:
                    return len(x)
                return 0
            """, "AS3202"),
        "wrong-short-circuit": ("""
            def read(x: Optional[str]) -> bool:
                return x != None or len(x) > 0
            """, "AS3202"),
        "wrong-payload": ("""
            def read() -> Optional[i64]:
                return "wrong"
            """, "AS3202"),
        "borrow-escape": ("""
            def read(x: Slice[i64]) -> Optional[Slice[i64]]:
                return x
            """, "AS3400"),
        "no-result-payload": ("""
            def read(x: Optional[None]) -> None:
                pass
            """, "AS3202"),
    }
    with tempfile.TemporaryDirectory(prefix="as-optional-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                diagnostic["code"] == code for diagnostic in report["diagnostics"]), (name, report)
        ir = work / "optional.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "optional", optimization)
            assert result.returncode == 0 and result.stdout == "native Optional ok\n", result
    print(f"PASS Optional: {len(cases)} diagnostics, O0/O2 flow, layout, dictionary lookup and roots")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-optional-control-") as temporary:
        work = Path(temporary)
        ir = work / "optional.ll"
        emit_ir(compiler, FIXTURE, ir)
        result = sanitized(ir, RUNTIME, work / "optional-ok", "-O0")
        assert result.returncode == 0, result
        # Delete only Optional payload scans. The fixture stores freshly
        # allocated text inside a boxed Header, returns from its maker, and
        # explicitly collects before reading it. ASan must observe freed text.
        pattern = r"(payload:\n  %value = getelementptr [^\n]+\n)  call void @scan\d+\(ptr %value\)\n"
        text, count = re.subn(pattern, r"\1", ir.read_text())
        assert count > 0, "Optional scanner mutation anchor changed"
        ir.write_text(text)
        result = sanitized(ir, RUNTIME, work / "optional-broken", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS negative control: omitted Optional payload roots observed as ASan use-after-free")


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
