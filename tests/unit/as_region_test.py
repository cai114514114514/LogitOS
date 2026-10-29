#!/usr/bin/env python3
"""Verify the native ownership backend independently of source-level checking.

These C probes cover runtime transitions, not static lifetime analysis. Source
move checking and emitted cleanup are covered by as_region_source_test.py.
"""

import argparse
from pathlib import Path
import tempfile

from as_managed_test import ROOT, sanitized
from as_runtime import copy_runtime

SOURCE = ROOT / "tests/unit/as_region_runtime_test.c"
UNITS = ("region.c", "region_borrow.c")
EXPECTED = {
    False: "native region runtime ok: 129 checks, tracked=0\n",
    True: "native region runtime ok: 144 checks, tracked=1\n",
}


def require_success(result, tracked):
    assert result.returncode == 0 and result.stdout == EXPECTED[tracked], result


def instrument(directory):
    copy_runtime(directory)
    for name in UNITS:
        path = directory / name
        text = path.read_text()
        text = text.replace("calloc(", "test_region_allocate(")
        text = text.replace("free(", "test_region_free(")
        anchor = "#include <stdlib.h>"
        assert text.count(anchor) == 1
        text = text.replace(anchor, anchor + "\nvoid *test_region_allocate(size_t, size_t);\n"
                            "void test_region_free(void *);")
        path.write_text(text)


def probe(work, runtime, name, optimization, tracked=False):
    source = runtime / "probe.c"
    source.write_text(SOURCE.read_text())
    return sanitized(source, runtime, work / name, optimization,
                     ("--tracked",) if tracked else ())


def exercise(work):
    runtime = work / "runtime"
    copy_runtime(runtime)
    for optimization in ("-O0", "-O2"):
        require_success(probe(work, runtime, "production", optimization), False)
    instrument(runtime)
    for optimization in ("-O0", "-O2"):
        result = probe(work, runtime, "tracked", optimization, True)
        require_success(result, True)
        print(result.stdout.strip(), optimization, "ASan/UBSan")


def controls(work):
    runtime = work / "runtime"
    instrument(runtime)
    require_success(probe(work, runtime, "baseline", "-O0", True), True)
    originals = {name: (runtime / name).read_text() for name in UNITS}
    mutations = (
        ("missing-free", "region.c", "test_region_free(owner);", "/* missing owner free */",
         "allocations == releases"),
        ("shared-write", "region.c", "!owner || owner->readers || owner->writer",
         "!owner || owner->writer", "at_region_write(owner, 2, 31) == AT_E_RUNTIME"),
        ("address-access", "region.c", "!owner || owner->writer || (writable && owner->readers)",
         "!owner || owner->writer",
         "at_region_address(&address, owner, 2, 1) == AT_E_RUNTIME && !address"),
        ("parent-access", "region_borrow.c", "(borrow->writable && borrow->children)",
         "0", "at_region_borrow_read(&value, parent, 1) == AT_E_RUNTIME"),
    )
    for name, unit, anchor, replacement, expected in mutations:
        assert originals[unit].count(anchor) == 1, (unit, anchor)
        for path, original in originals.items():
            (runtime / path).write_text(original)
        (runtime / unit).write_text(originals[unit].replace(anchor, replacement))
        result = probe(work, runtime, name, "-O0", True)
        assert result.returncode == 1 and expected in result.stdout, result
        assert "native region runtime ok" not in result.stdout, result
        print(f"PASS region control: {name} observed failing {expected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-region-runtime-") as directory:
        (controls if args.negative_control else exercise)(Path(directory))


if __name__ == "__main__":
    main()
