#!/usr/bin/env python3
"""Compare executed native constant values against independently compiled ABI data.

The frozen migration inventory supplies C identities, not values. An accidental
manifest rename, host syscall number, or missing registration must therefore
change real program output. The same expected output is used in the guest.
"""

import argparse
import json
import os
from pathlib import Path
import re
import tempfile
from textwrap import dedent

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized

FIXTURE = ROOT / "tests/fixtures/astyped/constants/main.as"
BASELINE = ROOT / "tests/fixtures/astyped/migration-builtins.json"


def expected_output(work):
    """Compile the target headers directly; do not read the compiler's table."""
    constants = json.loads(BASELINE.read_text())["constants"]
    statements = [f'    printf("{name} %lld\\n", (long long)({symbol}));'
                  for name, symbol in constants.items()]
    source = work / "abi-oracle.c"
    source.write_text('#include <stdio.h>\n#include "logit_abi.h"\n'
                      '#include "runtime/capability_bits.h"\nint main(void) {\n'
                      + "\n".join(statements) + "\n    return 0;\n}\n")
    binary = work / "abi-oracle"
    result = invoke([os.environ.get("CC", "clang"), source, "-o", binary,
                     "-I", ROOT / "include/abi", "-I", ROOT / "c/apps/as"])
    assert result.returncode == 0, result.stderr
    result = invoke([binary])
    assert result.returncode == 0 and len(result.stdout.splitlines()) == len(constants), result
    return result.stdout


def require_output(result, expected):
    assert result.returncode == 0 and result.stdout == expected, result
    assert not result.stderr, result.stderr


def check_completion(work):
    constants = json.loads(BASELINE.read_text())["constants"]
    names = ",\n".join(f'        "{name}"' for name in constants)
    source = work / "completion.c"
    source.write_text('''#include "editor/completion.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const char *names[] = {
''' + names + '''
    };
    for (unsigned index = 0; index < sizeof names / sizeof names[0]; index++) {
        const char *name = names[index];
        int length = (int)strlen(name);
        Completion candidates[512];
        int count = as_complete(name, length, length, candidates, 512);
        int found = 0;
        for (int candidate = 0; candidate < count; candidate++) {
            if (!strcmp(candidates[candidate].label, name) &&
                candidates[candidate].kind == CMP_BUILTIN) {
                found = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "missing completion: %s\\n", name);
            return 1;
        }
    }
    return 0;
}
''')
    binary = work / "completion"
    result = invoke([os.environ.get("CC", "clang"), source, "-o", binary,
                     ROOT / "c/apps/as/editor/completion.c", "-I", ROOT / "c/apps/as"])
    assert result.returncode == 0, result.stderr
    result = invoke([binary])
    assert result.returncode == 0, result.stderr


def exercise(compiler):
    cases = {
        "unknown": ("def main() -> i64:\n    return SYS_NOT_A_CALL\n", "AS3205"),
        "narrow": ("def main() -> u8:\n    return SYS_WRITE\n", "AS3202"),
        "signed": ("def main() -> u64:\n    return SYS_WRITE\n", "AS3202"),
        "uninitialized": ("""
            def main() -> i64:
                value = SYS_WRITE
                SYS_WRITE = 10
                return value
            """, "AS3206"),
    }
    with tempfile.TemporaryDirectory(prefix="as-constants-") as temporary:
        work = Path(temporary)
        expected = expected_output(work)
        check_completion(work)
        for name, (body, code) in cases.items():
            source = work / (name + ".as")
            source.write_text("# aether: 3.0\n" + dedent(body).lstrip("\n"))
            result = invoke([compiler, "check", source, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and any(
                item["code"] == code for item in report["diagnostics"]), (name, report)

        ir = work / "constants.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_output(sanitized(ir, RUNTIME, work / "constants", optimization), expected)

        # Link/run through the independent frontend too. It has no VM or old
        # constant registration translation unit to provide missing bindings.
        independent = compiler.parent / "as-native-link-test"
        emit_ir(independent, FIXTURE, ir)
        require_output(sanitized(ir, RUNTIME, work / "independent", "-O2"), expected)
    print(f"PASS native constants: {len(expected.splitlines())} ABI identities, "
          f"{len(cases)} diagnostics, binding precedence, generic copies and O0/O2")


def negative_control(compiler):
    with tempfile.TemporaryDirectory(prefix="as-constants-control-") as temporary:
        work = Path(temporary)
        expected = expected_output(work)
        ir = work / "constants.ll"
        emit_ir(compiler, FIXTURE, ir)
        require_output(sanitized(ir, RUNTIME, work / "positive", "-O0"), expected)

        # Change an emitted identity while leaving the name and all successful
        # exit paths intact. A banner or return-code-only test would miss this.
        text, count = re.subn(r"(call void @at_print_i64\(i64 )1\)",
                             r"\g<1>999)", ir.read_text(), count=1)
        assert count == 1, "constant output mutation anchor changed"
        ir.write_text(text)
        result = sanitized(ir, RUNTIME, work / "negative", "-O0")
        assert result.returncode == 0 and result.stdout != expected, result
        try:
            require_output(result, expected)
        except AssertionError:
            pass
        else:
            raise AssertionError("changed native constant escaped the output comparison")
    print("PASS constant control: changed native identity fails complete output comparison")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_control(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
