#!/usr/bin/env python3
"""Private source mutations must fail for the arithmetic/policy assertions."""

import os
from pathlib import Path
import subprocess
import tempfile

from as_compiler import ROOT, compiler_sources
from as_numeric_test import gate

MUTATIONS = [
    (
        "legacy/vm.c",
        "if (ckd_add(&result, AS_INT(a), AS_INT(b))) {",
        "if ((ckd_add(&result, AS_INT(a), AS_INT(b)), 0)) {",
        "source:",
    ),
    (
        "legacy/bytecode.c",
        "buf[8] != 2 || buf[9]",
        "(buf[8] != 2 && buf[8] != 3) || buf[9]",
        "unknown-bytecode-policy:",
    ),
]


def main():
    sources = compiler_sources()
    with tempfile.TemporaryDirectory(prefix="as-numeric-controls-") as temporary:
        directory = Path(temporary)
        for index, (filename, correct, incorrect, expected) in enumerate(MUTATIONS):
            original = ROOT / "c/apps/as" / filename
            source = original.read_text()
            if source.count(correct) != 1:
                raise RuntimeError("mutation setup drift: " + filename)

            # Replace only the selected translation unit in the build's source
            # inventory. The positive compiler and working files stay intact.
            mutant = directory / original.name
            mutant.write_text(source.replace(correct, incorrect))
            executable = directory / f"asc-{index}"
            command = [
                os.environ.get("CC", "clang"), "-O2",
                "-I" + str(ROOT / "c/apps/as"),
                "-I" + str(ROOT / "include/abi"),
                *[str(mutant if path == original else path) for path in sources],
                "-o", str(executable),
            ]
            subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)

            work = directory / f"work-{index}"
            work.mkdir()
            try:
                gate(executable, work)
            except AssertionError as error:
                if not str(error).startswith(expected):
                    raise
                print("PASS negative control:", filename, "failed at", expected[:-1])
            else:
                raise RuntimeError("gate accepted broken " + filename)


if __name__ == "__main__":
    main()
