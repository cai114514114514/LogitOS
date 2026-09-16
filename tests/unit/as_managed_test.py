#!/usr/bin/env python3
"""Native heap/rewritten library acceptance, including observed failing controls.

ASan checks host memory safety; the separate guest gate executes the same real
library fixture as AEX. Neither is evidence that all A2 capabilities migrated.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from as_runtime import copy_runtime, runtime_sources

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "c/apps/as/runtime"
LIBRARY = ROOT / "fsroot/as/lib"
FIXTURE = ROOT / "tests/fixtures/astyped/managed/main.as"


def invoke(command, **options):
    return subprocess.run(list(map(str, command)), capture_output=True, text=True,
                          timeout=120, **options)


def emit_ir(compiler, source, destination):
    result = invoke([compiler, "build", source, "--stdlib", LIBRARY,
                     "--emit-llvm", "-o", destination])
    assert result.returncode == 0, result.stdout + result.stderr


def sanitized(ir, runtime, binary, optimization, arguments=()):
    if ir.suffix == ".ll":
        # Clang's C frontend normally adds sanitize_address to definitions.
        # Hand-emitted LLVM bypasses that frontend: -fsanitize alone used to
        # instrument only the C runtime, leaving language field/index loads
        # unchecked. Explicitly mark this private test copy before the pass.
        instrumented = binary.with_suffix(".sanitized.ll")
        text, definitions = re.subn(r"^(define [^\n]+\)) \{", r"\1 sanitize_address {",
                                    ir.read_text(), flags=re.MULTILINE)
        assert definitions > 0, "native sanitizer function marker did not match"
        instrumented.write_text(text)
        ir = instrumented
    result = invoke([os.environ.get("CLANG", "clang"), optimization, "-g",
                     "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                     ir, *runtime_sources(runtime), "-o", binary])
    assert result.returncode == 0, result.stdout + result.stderr
    # LeakSanitizer is unavailable on Apple Silicon. The fixture independently
    # checks that managed payload bytes reach zero after all worker frames exit.
    environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1")
    return invoke([binary, *arguments], env=environment)


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-managed-controls-") as temporary:
        work = Path(temporary)
        ir = work / "managed.ll"
        emit_ir(compiler, FIXTURE, ir)
        copy_runtime(work)
        runtime = (RUNTIME / "heap.c").read_text()
        anchor = "void at_gc_collect(void)\n{"
        assert runtime.count(anchor) == 1, "collector mutation anchor changed"
        broken = work / "heap.c"
        broken.write_text(runtime.replace(anchor, anchor + "\n    return;"))
        result = sanitized(ir, work, work / "no-collection", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        assert "managed stdlib ok" not in result.stdout, result
        print("PASS negative control: disabled collection observed failing")

        # An earlier argument must remain alive while a later argument collects.
        # Remove only expression-root registrations from a private emitted IR;
        # locals/parameters and the runtime are unchanged. The exact positive
        # program must run first, then ASan must identify a real use-after-free.
        source = work / "arguments.as"
        source.write_text('''# aether: 3.0
def text() -> str:
    return "first " + "argument"
def collect() -> i64:
    gc_collect()
    return 7
def consume(value: str, number: i64) -> None:
    assert value == "first argument"
    assert number == 7
def main() -> None:
    consume(text(), collect())
''')
        emit_ir(compiler, source, ir)
        result = sanitized(ir, RUNTIME, work / "arguments-ok", "-O0")
        assert result.returncode == 0, result
        changed, replacements = re.subn(
            r"^  call void @at_gc_root\(ptr %root\d+, ptr %rootvalue\d+, ptr @scan\d+\)\n",
            "", ir.read_text(), flags=re.MULTILINE)
        assert replacements > 0, "temporary-root mutation anchor changed"
        ir.write_text(changed)
        result = sanitized(ir, RUNTIME, work / "arguments-broken", "-O0")
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
        print("PASS negative control: missing argument roots observed as ASan use-after-free")


def exercise(compiler):
    cases = {
        "heterogeneous": ('def main() -> None:\n    items = [1, "x"]\n', "AS3202"),
        "empty-context": ("def main() -> None:\n    items = []\n", "AS3900"),
        "append-type": ('def main() -> None:\n    items: List[i64] = []\n    items.append("x")\n', "AS3202"),
        "void-element": ("def noop() -> None:\n    pass\ndef main() -> None:\n    items = [noop()]\n", "AS3202"),
        "immutable-string": ('def main() -> None:\n    text = "abc"\n    text[0] = "x"\n', "AS3400"),
        "slice-escape": ("def save(items: List[Slice[i64]], view: Slice[i64]) -> None:\n    items.append(view)\n", "AS3400"),
        "bad-method": ('def main() -> None:\n    print("x".slice("x", 2))\n', "AS3202"),
        "missing-method-argument": ('def main() -> None:\n    print("x".slice(1))\n', "AS3204"),
    }
    with tempfile.TemporaryDirectory(prefix="as-managed-") as temporary:
        work = Path(temporary)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + body)
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                diagnostic["code"] == code for diagnostic in report["diagnostics"]), (name, report)

        ir = work / "managed.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "managed", optimization)
            assert result.returncode == 0, result.stdout + result.stderr
            assert result.stdout == "managed stdlib ok\n", result
            print(f"PASS managed standard library, GC roots and reclamation {optimization} ASan/UBSan")

        emit_ir(compiler, ROOT / "tests/fixtures/astyped/numeric-lib/main.as", ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "numeric-lib", optimization)
            assert result.returncode == 0 and result.stdout == "native numeric library ok\n", result
            print(f"PASS native mathx/random API, errors and seeded sequence {optimization} ASan/UBSan")

        # Both checker and build must read library overlays into their snapshot.
        overlay = work / "strings-overlay.as"
        overlay.write_text((LIBRARY / "strings.as").read_text().replace(
            'return s.find(needle)', 'return missing_symbol'))
        result = invoke([compiler, "check", FIXTURE, "--json", "--stdlib", LIBRARY,
                         "--overlay", LIBRARY / "strings.as", overlay])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(d["code"] == "AS3205" for d in report["diagnostics"]), report
        # A local module has priority, including an unfinished/broken overlay.
        entry = work / "main.as"
        entry.write_text("# aether: 3.0\nimport strings\ndef main() -> None:\n    strings.local()\n")
        local = work / "strings.as"
        local.write_text("# aether: 3.0\ndef local() -> None:\n    pass\n")
        result = invoke([compiler, "check", entry, "--stdlib", LIBRARY, "--json"])
        assert result.stdout, (result.returncode, result.stderr)
        report = json.loads(result.stdout)
        assert result.returncode == 0, report
        assert str(local) in [source["path"] for source in report["sources"]], report
    print(f"PASS {len(cases)} managed diagnostics, library snapshots and local precedence")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())


if __name__ == "__main__":
    main()
