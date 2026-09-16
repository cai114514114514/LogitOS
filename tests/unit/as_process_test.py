#!/usr/bin/env python3
"""Native argv copying, CLI argument separation and collector count semantics."""

import argparse
import json
from pathlib import Path
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/process/main.as"
ARGUMENTS = ["中文", "two words", "", "--json"]
EXPECTED = "native arguments and collection counts ok\n"


def require_output(result):
    assert result.returncode == 0 and result.stdout == EXPECTED, result


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-process-") as temporary:
        work = Path(temporary)
        ir = work / "process.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(sanitized(ir, RUNTIME, work / "process", optimization, ARGUMENTS))

        # Both the actual executable and `as run --` must preserve empty and
        # option-looking arguments without reinterpreting them as compiler flags.
        result = invoke([compiler, "run", FIXTURE, "--toolchain", RUNTIME,
                         "--json", "--", *ARGUMENTS])
        report = json.loads(result.stdout)
        assert result.returncode == 0 and report["output"] == EXPECTED, report

        for name in ("args", "gc", "gc_stats"):
            source = work / (name + ".as")
            source.write_text(f"# aether: 3.0\ndef main() -> None:\n    {name}(1)\n")
            checked = invoke([compiler, "check", source, "--json"])
            report = json.loads(checked.stdout)
            assert checked.returncode == 1 and any(
                error["code"] == "AS3204" for error in report["diagnostics"]), report
    print("PASS process: argv snapshots, CLI separator, collector counts, O0/O2 ASan")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-process-control-") as temporary:
        work = Path(temporary)
        ir = work / "process.ll"
        emit_ir(compiler, FIXTURE, ir)
        copy_runtime(work)

        # Force a safepoint on every allocation so a missing construction root
        # cannot accidentally survive until the normal allocation threshold.
        heap = work / "heap.c"
        text = heap.read_text()
        anchor = "if (live_bytes + bytes >= collect_at)"
        assert text.count(anchor) == 1, "collector threshold anchor changed"
        heap.write_text(text.replace(anchor, "if (1)"))
        require_output(sanitized(ir, work, work / "positive", "-O0", ARGUMENTS))

        process = work / "process.c"
        text = process.read_text()
        anchor = "at_gc_root(&list_root, &list, scan_list_pointer);"
        assert text.count(anchor) == 1, "argument list root anchor changed"
        process.write_text(text.replace(anchor, "/* deliberately missing list root */"))
        result = sanitized(ir, work, work / "negative", "-O0", ARGUMENTS)
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
    print("PASS process control: missing argument construction root fails under ASan")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
