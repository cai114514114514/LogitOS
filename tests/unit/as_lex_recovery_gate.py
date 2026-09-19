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

    # A4-F3: drop the `**=` longest-match step inside the `**` branch and the
    # compound battery above must redden -- this is the watch-it-fail half of
    # the new probes, so the C lexer cannot silently split `**=` into `**` `=`.
    anchor = "push(&b, T_POWEQ, s, 3, line);"
    assert text.count(anchor) == 1, "poweq mutation anchor changed"
    with tempfile.TemporaryDirectory(prefix="as-lex-poweq-") as temporary:
        directory = Path(temporary)
        mutant = directory / "lexer.c"
        mutant.write_text(text.replace(anchor, "push(&b, T_POW, s, 2, line);"))
        executable = directory / "test"
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
                        "-fsanitize=address,undefined", "-I" + str(ROOT / "c/apps/as"),
                        str(ROOT / "tests/unit/as_lex_recovery_test.c"), str(mutant),
                        "-o", str(executable)], check=True, capture_output=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
        assert result.returncode == 1 and "FAIL compound probe longest match" in result.stderr, result
    print("PASS negative control: `**=` without its longest-match step splits into `**` `=`")


if __name__ == "__main__":
    main()
