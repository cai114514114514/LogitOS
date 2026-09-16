#!/usr/bin/env python3
"""Require the generic gate to reject a compiler that ignores constraints."""

import os
from pathlib import Path
import subprocess
import tempfile

from as_generic_test import ROOT, exercise_constraints
from as_compiler import compiler_sources


def main():
    original = ROOT / "c/apps/as/sema/generic.c"
    source = original.read_text()
    correct = "if (constraint == AT_CONSTRAINT_NONE) {"
    incorrect = ("if (constraint == AT_CONSTRAINT_NONE || "
                 "project->types[type].kind == AT_PARAMETER) {")
    assert source.count(correct) == 1, "generic constraint mutation anchor changed"

    # Compile the altered translation unit in a private directory. This proves
    # the oracle catches an unconstrained template even without any call site.
    with tempfile.TemporaryDirectory(prefix="as-generic-negative-") as directory:
        directory = Path(directory)
        mutant = directory / original.name
        mutant.write_text(source.replace(correct, incorrect))
        executable = directory / "asc"
        sources = [mutant if path == original else path for path in compiler_sources()]
        command = [
            os.environ.get("CC", "clang"), "-O1",
            "-I" + str(ROOT / "c/apps/as"),
            "-I" + str(ROOT / "include/abi"),
            *map(str, sources), "-o", str(executable),
        ]
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=120)
        try:
            exercise_constraints(executable)
        except AssertionError as error:
            assert str(error) == "unconstrained-body", f"Wrong failure: {error}"
        else:
            raise AssertionError("Generic gate accepted arithmetic without a numeric constraint")
    print("PASS negative control: unconstrained-body observed failing")


if __name__ == "__main__":
    main()
