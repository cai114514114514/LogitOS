#!/usr/bin/env python3
"""Prove that the native gate detects an invalid definite-assignment merge."""

import os
from pathlib import Path
import subprocess
import tempfile

from as_typed_test import ROOT, exercise
from as_compiler import compiler_sources


def main():
    original = ROOT / "c/apps/as/sema/check.c"
    source = original.read_text()
    correct = "after_then[i] & function->locals[i].initialized"
    incorrect = "after_then[i] || function->locals[i].initialized"
    assert source.count(correct) == 1, "definite-assignment mutation anchor changed"

    # Mutate a private translation unit. The working compiler source and the
    # executable used by the positive gate remain untouched.
    with tempfile.TemporaryDirectory(prefix="as-typed-negative-") as directory:
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
            exercise(executable, directory)
        except AssertionError as error:
            assert str(error) == "branch-initialization", f"Wrong failure: {error}"
        else:
            raise AssertionError("Native gate accepted a variable initialized in only one branch")
    print("PASS negative control: branch-initialization observed failing")


if __name__ == "__main__":
    main()
