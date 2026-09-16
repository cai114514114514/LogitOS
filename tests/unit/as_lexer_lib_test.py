#!/usr/bin/env python3
"""Native aslex corpus parity, source spans and observed failing token control."""

import argparse
import json
import os
from pathlib import Path
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT

FIXTURE = ROOT / "tests/fixtures/astyped/aslex/main.as"
LIBRARY = ROOT / "fsroot/as/lib/aslex.as"
DRIVER = ROOT / "tests/unit/aslexdump.as"

# Hex encoding makes embedded quotes, escapes and UTF-8 bytes unambiguous in
# the comparison. The expected stream comes from the independently compiled C
# frontend, not from running the implementation under test a second time.
DUMP = '''# aether: 3.0
from aslex import lex
def dump(source: str) -> None:
    digits = "0123456789abcdef"
    for token in lex(source):
        text = cast[str](token[1])
        encoded = ""
        for index in range(len(text)):
            byte = ord(text[index])
            encoded += digits[byte / 16] + digits[byte % 16]
        print(str(cast[i64](token[0])) + " " + str(cast[i64](token[2])) + " " + str(len(text)) + " " + encoded)
'''


def corpus_program(work):
    oracle = work / "lexer-oracle"
    compiled = invoke([os.environ.get("CLANG", "clang"), "-O2", "-Wall", "-Wextra",
                       "-I", ROOT / "c/apps/as", ROOT / "tests/unit/as_lexer_oracle.c",
                       ROOT / "c/apps/as/frontend/lexer.c", "-o", oracle])
    assert compiled.returncode == 0, compiled
    sources = sorted((ROOT / "fsroot/as/lib").glob("*.as"))
    sources += sorted((ROOT / "fsroot/as/examples").glob("*.as"))
    # 56, not 59. The three that left are the A2 toolchain retired with the
    # engine: fsroot/as/lib/asc.as (the AetherScript-written A2 compiler),
    # examples/selfhost.as (which imported it) and examples/ascbench.as (which
    # timed `as -c` producing bytecode). Lowering a ratchet needs a reason and
    # this is it -- the corpus did not lose coverage of anything A3 can express.
    # Every remaining source is `# aether: 3.0`, which was not true at 59.
    assert len(sources) >= 56, "lexer corpus unexpectedly shrank"
    program = [DUMP, "def main() -> None:"]
    expected = []
    for index, source in enumerate(sources):
        result = invoke([oracle, source])
        assert result.returncode == 0, (source, result)
        marker = f"source {index}: {source.relative_to(ROOT)}"
        program.append("    print(" + json.dumps(marker) + ")")
        # A3 text literals accept UTF-8 and ordinary escaped control characters.
        program.append("    dump(" + json.dumps(source.read_text(), ensure_ascii=False) + ")")
        program.append("    gc_collect()")
        expected.append(marker + "\n" + result.stdout)
    entry = work / "corpus.as"
    entry.write_text("\n".join(program) + "\n")
    return entry, "".join(expected), len(sources)


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-lexer-lib-") as temporary:
        work = Path(temporary)
        ir = work / "fixture.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "fixture", optimization)
            assert result.returncode == 0 and result.stdout == "native lexer library ok\n", result
        entry, expected, count = corpus_program(work)
        emit_ir(compiler, entry, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "corpus", optimization)
            assert result.returncode == 0, result
            assert result.stdout == expected, "native/C lexer token stream differs"
    print(f"PASS native lexer: {count} complete source streams, spans/errors/GC, O0/O2 ASan")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-lexer-control-") as temporary:
        work = Path(temporary)
        entry = work / "main.as"
        entry.write_text(FIXTURE.read_text())
        library = work / "aslex.as"
        library.write_text(LIBRARY.read_text())
        ir = work / "lexer.ll"
        emit_ir(compiler, entry, ir)
        result = sanitized(ir, RUNTIME, work / "positive", "-O0")
        assert result.returncode == 0, result
        text = library.read_text()
        anchor = "self.emit(T_STR, self.src.sub(cs, self.i))"
        assert text.count(anchor) == 1, "string token mutation anchor changed"
        library.write_text(text.replace(anchor, "self.emit(T_IDENT, self.src.sub(cs, self.i))"))
        emit_ir(compiler, entry, ir)
        result = sanitized(ir, RUNTIME, work / "negative", "-O0")
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
    print("PASS native lexer control: wrong string token observed failing")


def driver_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-lexer-driver-") as temporary:
        work = Path(temporary)
        ir = work / "driver.ll"
        source = ROOT / "tests/fixtures/astyped/numeric/main.as"
        oracle = invoke([compiler, "-lex", source])
        assert oracle.returncode == 0 and oracle.stdout, oracle
        emit_ir(compiler, DRIVER, ir)
        for optimization in ("-O0", "-O2"):
            executable = work / "driver"
            result = sanitized(ir, RUNTIME, executable, optimization, [source])
            assert result.returncode == 0 and result.stdout == oracle.stdout, result
            usage = invoke([executable])
            assert usage.returncode == 2 and usage.stdout == "usage: aslexdump SOURCE\n", usage
            missing = invoke([executable, work / "missing.as"])
            assert missing.returncode == 1 and "IOError" in missing.stderr, missing
            invalid = work / "invalid-utf8.as"
            invalid.write_bytes(b"\xff")
            decoded = invoke([executable, invalid])
            assert decoded.returncode == 1 and "ConversionError" in decoded.stderr, decoded

        contents = DRIVER.read_text()
        assert contents.count("checksum = 0") == 1, "driver mutation anchor changed"
        mutant = work / "driver.as"
        mutant.write_text(contents.replace("checksum = 0", "checksum = 1"))
        emit_ir(compiler, mutant, ir)
        result = sanitized(ir, RUNTIME, work / "negative", "-O0", [source])
        assert result.returncode == 0, result
        assert result.stdout != oracle.stdout, "token oracle accepted a broken driver checksum"
        print("PASS native lexer tool control: wrong checksum observed differing from C oracle")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if args.negative_control:
        negative_control(compiler)
        driver_control(compiler)
    else:
        exercise(compiler)
