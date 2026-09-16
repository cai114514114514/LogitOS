#!/usr/bin/env python3
"""Watch the recovery-token oracle reject a private fail-fast scanner mutation."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()
    subprocess.run([str(args.binary.resolve())], check=True, timeout=20)
    if not args.negative:
        return
    original = ROOT / "c/apps/as/frontend/lexer.c"
    text = original.read_text()
    guard = "if (err && report && !error->out_of_memory) {"
    assert text.count(guard) == 1, "recovery guard changed"
    with tempfile.TemporaryDirectory(prefix="as-lex-recovery-") as temporary:
        directory = Path(temporary)
        mutant = directory / "lexer.c"
        mutant.write_text(text.replace(guard, "if (0) {"))
        executable = directory / "test"
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
                        "-fsanitize=address,undefined", "-I" + str(ROOT / "c/apps/as"),
                        str(ROOT / "tests/unit/as_lex_recovery_test.c"), str(mutant),
                        "-o", str(executable)], check=True, capture_output=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
        assert result.returncode == 1 and "FAIL recovery keeps token stream" in result.stderr, result
    print("PASS negative control: fail-fast lexer loses recovered token stream")


if __name__ == "__main__":
    main()
