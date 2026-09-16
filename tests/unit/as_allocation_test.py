#!/usr/bin/env python3
"""Manual native allocation, failure propagation and actual release gates.

Private runtime copies instrument only manual allocation calls. This observes
real frees even on macOS, where ASan has no leak detector. All pointer accesses
stay in owned live storage; controls fail assertions, not memory corruption.
"""

import argparse
import json
from pathlib import Path
import tempfile

from as_managed_test import ROOT, RUNTIME, emit_ir, sanitized, invoke
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/allocation"


def require_output(result, expected):
    assert result.returncode == 0 and result.stdout == expected + "\n", result


def instrument_runtime(directory):
    copy_runtime(directory)
    path = directory / "allocation.c"
    source = path.read_text()
    source = source.replace("calloc(1,", "test_allocate(1,")
    source = source.replace("free(allocation);", "test_release(allocation);")
    anchor = "#include <stdlib.h>"
    assert source.count(anchor) == 1
    source = source.replace(anchor, anchor + "\nvoid *test_allocate(size_t, size_t);\n"
                            "void test_release(void *);")
    path.write_text(source)
    return source


def runtime_probe(work, runtime, name, optimization):
    # The probe needs the headers from this exact private runtime copy. Keeping
    # it there also prevents accidental compilation against a stale toolchain.
    source = runtime / "probe.c"
    source.write_text((FIXTURE / "runtime.c").read_text())
    return sanitized(source, runtime, work / name, optimization)


def exercise(compiler, work):
    cases = {
        "allocate-outside": ("def main() -> None:\n    alloc(8)", "AS3810"),
        "release-outside": ("def f(p: Ptr[u8]) -> None:\n    dealloc(p)", "AS3810"),
        "allocate-arity": ("def main() -> None:\n    unsafe:\n        alloc()", "AS3204"),
        "release-arity": ("def main() -> None:\n    unsafe:\n        dealloc()", "AS3204"),
        "float-size": ("def main() -> None:\n    unsafe:\n        alloc(1.5)", "AS3202"),
        "managed-release": ("def main() -> None:\n    unsafe:\n        dealloc(buffer(8))", "AS3202"),
        "integer-release": ("def main() -> None:\n    unsafe:\n        dealloc(0)", "AS3202"),
        "signed-result": ("def main() -> None:\n    unsafe:\n        p: Ptr[i8] = alloc(8)", "AS3202"),
    }
    for name, (body, code) in cases.items():
        source = work / (name + ".as")
        source.write_text("# aether: 3.0\n" + body + "\n")
        result = invoke([compiler, "check", source, "--json"])
        report = json.loads(result.stdout)
        assert result.returncode == 1 and any(
            item["code"] == code for item in report["diagnostics"]), (name, report)

    ir = work / "allocation.ll"
    emit_ir(compiler, FIXTURE / "main.as", ir)
    for optimization in ("-O0", "-O2"):
        require_output(sanitized(ir, RUNTIME, work / "allocation", optimization),
                       "native manual allocation ok")
    emit_ir(compiler, FIXTURE / "failure.as", ir)
    for optimization in ("-O0", "-O2"):
        result = sanitized(ir, RUNTIME, work / "failure", optimization)
        assert result.returncode == 1 and "ValueError" in result.stderr, result
        assert str(FIXTURE / "failure.as") + ":5:" in result.stderr, result

    runtime = work / "runtime"
    instrument_runtime(runtime)
    for optimization in ("-O0", "-O2"):
        require_output(runtime_probe(work, runtime, "runtime-probe", optimization),
                       "native allocation runtime ok")

    # Language return and exception paths must really call free. An assertion
    # on the copied text alone would pass even if these paths leaked memory.
    (runtime / "tracker.c").write_text((FIXTURE / "tracker.c").read_text())
    manifest = runtime / "sources.def"
    manifest.write_text(manifest.read_text() + "AT_RUNTIME_SOURCE(tracker)\n")
    emit_ir(compiler, FIXTURE / "main.as", ir)
    for optimization in ("-O0", "-O2"):
        require_output(sanitized(ir, runtime, work / "balanced", optimization),
                       "native manual allocation ok\nnative allocation balance ok")

    heap = runtime / "heap.c"
    text = heap.read_text()
    anchor = "void at_gc_collect(void)\n{"
    assert text.count(anchor) == 1
    heap.write_text(text.replace(anchor, anchor + "\n    static int calls;\n"
                                "    at_caps_set(++calls == 1 ? 0 : AS_CAP_RAW, NULL);"))
    emit_ir(compiler, FIXTURE / "revoked.as", ir)
    for optimization in ("-O0", "-O2"):
        require_output(sanitized(ir, runtime, work / "revoked", optimization),
                       "native allocation revocation ok\nnative allocation balance ok")

    failure_runtime = work / "oom-runtime"
    copy_runtime(failure_runtime)
    allocation = failure_runtime / "allocation.c"
    text = allocation.read_text()
    anchor = "calloc(1, sizeof *allocation + (size_t)bytes)"
    assert text.count(anchor) == 1
    allocation.write_text(text.replace(anchor, "NULL"))
    emit_ir(compiler, FIXTURE / "oom.as", ir)
    for optimization in ("-O0", "-O2"):
        require_output(sanitized(ir, failure_runtime, work / "oom", optimization),
                       "native allocation failure ok")
    print(f"PASS allocation: {len(cases)} diagnostics, zeroing, GC independence, cleanup, "
          "failure, revocation, provenance and real frees, O0/O2 ASan/UBSan")


def negative_controls(work):
    runtime = work / "runtime"
    original = instrument_runtime(runtime)
    require_output(runtime_probe(work, runtime, "baseline", "-O0"),
                   "native allocation runtime ok")
    path = runtime / "allocation.c"
    controls = {
        "no-release": ("test_release(allocation);", "/* deliberately skip the real free */"),
        "wrong-zeroing": ("allocation->next = allocations;",
                          "allocation->data[0] = 1;\n    allocation->next = allocations;"),
    }
    for name, (anchor, replacement) in controls.items():
        assert original.count(anchor) == 1
        path.write_text(original.replace(anchor, replacement))
        result = runtime_probe(work, runtime, name, "-O0")
        assert result.returncode != 0 and "Assertion" in result.stderr, result
        assert "native allocation runtime ok" not in result.stdout, result
        print(f"PASS allocation control: {name} observed failing its independent assertion")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-native-allocation-") as directory:
        work = Path(directory)
        if args.negative_control:
            negative_controls(work)
        else:
            exercise(args.compiler.resolve(), work)


if __name__ == "__main__":
    main()
