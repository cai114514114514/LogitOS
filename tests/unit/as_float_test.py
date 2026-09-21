#!/usr/bin/env python3
"""Float power semantics: IEEE llvm.pow edges, refusal doors, and a compiler
mutation control that turns pow into multiply and must fail the fixture."""

import argparse
import json
import os
from pathlib import Path
import tempfile

from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, LIBRARY
from as_compiler import ROOT, compiler_sources

FIXTURE = ROOT / "tests/fixtures/astyped/numeric/main.as"
OUTPUT = "中文 native 30 4\n"


def gate(compiler, work):
    compiler = Path(compiler).resolve()
    work = Path(work)

    def require(name, condition, details=""):
        if not condition:
            raise AssertionError(name + ": " + details)

    # Doors that must stay shut after float ** opened: mixed-type operands,
    # compound **= on floats (Integer constraint set), power on a bare Number
    # generic parameter (section 2 of LANGUAGE_3.md puts 乘方 under Integer).
    cases = {
        "power-mixed": 'x = 2.0 ** 2',
        "power-compound": 'x = 2.0\n    x **= 2.0',
        "power-number-generic": None,
    }
    with tempfile.TemporaryDirectory(prefix="as-float-") as temporary:
        temp = Path(temporary)
        for name, body in cases.items():
            source = temp / (name + ".as")
            if body is None:
                source.write_text(
                    "# aether: 3.0\n"
                    "def square[T: Number](value: T) -> T:\n"
                    "    return value ** value\n"
                    "def main() -> None:\n"
                    "    print(square(2.0))\n")
            else:
                source.write_text("# aether: 3.0\ndef main() -> None:\n    " + body + "\n")
            report = invoke([compiler, "check", source, "--json"])
            diagnostics = json.loads(report.stdout)["diagnostics"]
            require(name + " AS3202",
                    report.returncode == 1 and any(d["code"] == "AS3202" for d in diagnostics),
                    report.stdout + report.stderr)

    ir = work / "numeric.ll"
    emit_ir(compiler, FIXTURE, ir)
    for optimization in ("-O0", "-O2"):
        result = sanitized(ir, RUNTIME, work / f"numeric-{optimization[2:]}", optimization)
        require(f"pow-fixture {optimization}",
                result.returncode == 0 and result.stdout == OUTPUT,
                f"rc={result.returncode} out={result.stdout!r} err={result.stderr!r}")

    # Guest link proof: constant exponents fold away at -O2, so this program
    # takes the exponent from args() -- the x86 object then carries an
    # undefined `pow` libcall that the freestanding archive must satisfy.
    # aether-toolchain/libc.a gained musl libm on 2026-09-20 for exactly this;
    # without it ld.lld fails with "undefined symbol: pow" (watched red).
    libcall = work / "pow-libcall.as"
    libcall.write_text("# aether: 3.0\n"
                       "def main() -> i64:\n"
                       "    argv = args()\n"
                       "    print(parse_float(argv[1]) ** parse_float(argv[2]))\n"
                       "    return 0\n")
    result = invoke([compiler, "build", libcall, "--stdlib", LIBRARY,
                     "--target", "logitos-x86_64", "-o", work / "pow-libcall.aex"])
    require("guest pow link", result.returncode == 0, result.stdout + result.stderr)
    return len(cases) + 3


def exercise(compiler):
    count = gate(compiler, Path(tempfile.mkdtemp(prefix="as-float-run-")))
    print(f"PASS float power: {count} checks, 3 refusal doors, O0/O2 fixture")


def negative_control(compiler):
    sources = compiler_sources()
    original = ROOT / "c/apps/as/backend/llvm/numeric.c"
    correct = ('at_ir_emit(g, "  %s = call double @llvm.pow.f64(double %s, double %s)\\n", result.text,\n'
               '                       base.text, raised.text);')
    incorrect = 'result = at_ir_numeric(g, T_STAR, base, raised, n, wrap);'
    source = original.read_text()
    if source.count(correct) != 1:
        raise RuntimeError("mutation setup drift: numeric.c float pow emit line")

    with tempfile.TemporaryDirectory(prefix="as-float-control-") as temporary:
        directory = Path(temporary)
        mutant = directory / "numeric.c"
        mutant.write_text(source.replace(correct, incorrect))
        executable = directory / "asc-pow-is-mul"
        command = [
            os.environ.get("CC", "clang"), "-O2",
            "-I" + str(ROOT / "c/apps/as"),
            "-I" + str(ROOT / "include/abi"),
            *[str(mutant if path == original else path) for path in sources],
            "-o", str(executable),
        ]
        invoke(command, cwd=ROOT)

        work = directory / "work"
        work.mkdir()
        try:
            gate(executable, work)
        except AssertionError as error:
            if not str(error).startswith("pow-fixture"):
                raise
            print("PASS negative control: pow-lowered-as-multiply failed at", str(error).split(":")[0])
        else:
            raise RuntimeError("gate accepted pow-as-multiply compiler")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    arguments = parser.parse_args()
    if arguments.negative_control:
        negative_control(arguments.compiler)
    else:
        exercise(arguments.compiler)


if __name__ == "__main__":
    main()
