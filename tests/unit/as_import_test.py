#!/usr/bin/env python3
"""Native import identity, aliases and bounded compiler type arena acceptance.

Use distinct local/library contents so a wrong lookup can never pass just
because both files happen to define the same name with the same value.
"""

import argparse
import json
import os
from pathlib import Path
import re
import tempfile

from as_compiler import compiler_sources
from as_managed_test import emit_ir, invoke, sanitized, RUNTIME, ROOT, LIBRARY

FIXTURE = ROOT / "tests/fixtures/astyped/imports/main.as"


def checked(compiler, source, *arguments):
    result = invoke([compiler, "check", source, "--json", "--stdlib", LIBRARY, *arguments])
    assert result.stdout.startswith("{"), (result.returncode, result.stdout, result.stderr)
    report = json.loads(result.stdout)
    assert result.returncode == (0 if report["ok"] else 1), result
    return report


def imports(compiler, work):
    report = checked(compiler, FIXTURE)
    assert report["ok"], report
    paths = [entry["path"] for entry in report["sources"]]
    # Module and from-import aliases must share one source/module initializer.
    assert paths.count(str(LIBRARY / "strings.as")) == 1, paths
    assert str(FIXTURE.parent / "strings.as") in paths, paths
    ir = work / "imports.ll"
    emit_ir(compiler, FIXTURE, ir)
    for optimization in ("-O0", "-O2"):
        result = sanitized(ir, RUNTIME, work / "imports", optimization)
        assert result.returncode == 0 and result.stdout == "native imports ok\n", result

    # An unsaved library snapshot must participate in explicit imports too.
    overlay = work / "strings-overlay.as"
    overlay.write_text((LIBRARY / "strings.as").read_text() + "\n# 未保存的标准库\n")
    report = checked(compiler, FIXTURE, "--overlay", LIBRARY / "strings.as", overlay)
    assert report["ok"], report
    source = next(item for item in report["sources"] if item["path"] == str(LIBRARY / "strings.as"))
    assert source["bytes"] == len(overlay.read_bytes()), source

    entry = work / "main.as"
    (work / "absent.as").write_text("# aether: 3\ndef answer() -> i64:\n    return 9\n")
    cases = {
        "no-local-fallback": "import std.absent\n",
        "private-member": "from std.strings import _space as space\n",
        "missing-member": "from std.strings import absent as present\n",
        "duplicate-alias": "import std.strings as text\nimport std.paths as text\n",
        "incomplete-path": "import std.\n",
        "incomplete-alias": "import std.strings as\n",
        "long-name": "import " + "a" * 65 + "\n",
    }
    for name, declaration in cases.items():
        entry.write_text("# aether: 3\n" + declaration + "def main() -> None:\n    pass\n")
        report = checked(compiler, entry)
        assert not report["ok"] and report["diagnostics"], (name, report)
        assert len(report["diagnostics"]) <= 3, (name, report)


def capacity(compiler, work):
    header = (ROOT / "c/apps/as/ir/model.h").read_text()
    limit = int(re.search(r"^#define AT_TYPES (\d+)$", header, re.MULTILINE)[1])
    # Fill the arena with nominal declarations, then fail a generic signature
    # with a nested suite. Its body must not become module-level statements.
    declarations = [f"struct Item{index}:\n    x: i64\n" for index in range(limit + 2)]
    source = work / "capacity.as"
    source.write_text("# aether: 3\n" + "\n".join(declarations) + """
def too_late[T](value: T) -> T:
    if True:
        return value
    return value

def after(value: Missing) -> None:
    pass
""")
    report = checked(compiler, source)
    assert not report["ok"], report
    codes = [item["code"] for item in report["diagnostics"]]
    assert sorted(codes) == ["AS3200", "AS3600"], ("capacity-recovery", codes)
    assert not report.get("truncated"), report


def negative_control(compiler, work):
    imports(compiler, work)
    capacity(compiler, work)
    mutations = [
        ("frontend/import.c", "if (standard || (!source_exists(parser->p, path) && parser->p->library[0])) {",
         "if (!source_exists(parser->p, path) && parser->p->library[0]) {", imports),
        ("frontend/parser.c", "r->f = parent;\n        recover_declaration(r);",
         "r->f = parent;\n        recover(r);", capacity),
    ]
    for index, (relative, anchor, replacement, exercise) in enumerate(mutations):
        original = ROOT / "c/apps/as" / relative
        contents = original.read_text()
        assert contents.count(anchor) == 1, relative
        mutant = work / f"mutant{index}.c"
        mutant.write_text(contents.replace(anchor, replacement))
        binary = work / f"compiler{index}"
        sources = [mutant if path == original else path for path in compiler_sources()]
        result = invoke([os.environ.get("CC", "clang"), "-O1", "-I" + str(ROOT / "c/apps/as"),
                         "-I" + str(ROOT / "include/abi"), *sources, "-o", binary])
        assert result.returncode == 0, result.stderr
        try:
            exercise(binary, work)
        except AssertionError as error:
            if exercise is capacity:
                assert error.args[0][0] == "capacity-recovery", error
            else:
                # The mutant resolves std.strings to our local strings module;
                # it must fail checking the missing standard-library members.
                report = error.args[0]
                assert isinstance(report, dict) and not report["ok"], error
                assert any(item["code"] == "AS3300" for item in report["diagnostics"]), error
            print(f"PASS import control: {relative} regression observed failing")
        else:
            raise AssertionError(f"Import gate accepted broken {relative}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-imports-") as directory:
        work = Path(directory)
        if args.negative_control:
            negative_control(args.compiler.resolve(), work)
        else:
            imports(args.compiler.resolve(), work)
            capacity(args.compiler.resolve(), work)
            print("PASS imports: aliases, shadowing, overlays, diagnostics, capacity recovery, O0/O2 ASan")


if __name__ == "__main__":
    main()
