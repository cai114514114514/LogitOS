#!/usr/bin/env python3
"""A3 module completions share imports, declarations and source identities."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from as_compiler import ROOT, compiler_sources
from as_snapshot_test import encode

MODULE = '''# aether: 3.0
value: i64 = 7
_private_value: i64 = 9
struct Point:
    x: f64
class Document:
    name: str
def public() -> i64:
    return value
def _hidden() -> i64:
    return 0
'''


def query(compiler, source, module=MODULE, extra=(), offset=None):
    caret = len(source.encode()) if offset is None else offset
    payload = encode([("/project/main.as", source), ("/project/m.as", module), *extra])
    result = subprocess.run([str(compiler), "complete", "/project/main.as", "--snapshot-stdin",
                             "--at", str(caret)], input=payload, capture_output=True, timeout=15)
    assert result.returncode == 0 and not result.stderr, result
    report = json.loads(result.stdout)
    assert report["source_bytes"] == len(source.encode()) and report["caret"] == caret, report
    for item in report["items"]:
        original = dict([(path, text) for path, text in extra] + [
            ("/project/m.as", module), ("/project/main.as", source)])
        text = original[item["path"]].encode()
        assert text[item["start"]:item["end"]].decode() == item["name"], item
    return report


def exercise(compiler):
    prefix = "# aether: 3.0\nimport m as tools\ndef main() -> None:\n    "
    expected = {"public", "value", "Point", "Document"}
    report = query(compiler, prefix + "tools.")
    assert report["handled"] and {x["name"] for x in report["items"]} == expected, (
        "module exports", report)
    assert report["replace_start"] == report["caret"] and not report["truncated"]
    narrowed = query(compiler, prefix + "tools.pu")
    assert [x["name"] for x in narrowed["items"]] == ["public"], narrowed
    assert narrowed["replace_start"] == narrowed["caret"] - 2
    for member in ("public", "value", "Point", "Document"):
        source = prefix + "tools." + member + "\n"
        answer = query(compiler, source, offset=len(source.encode()) - 1)
        assert [item["name"] for item in answer["items"]] == [member], answer
    assert not query(compiler, prefix + "tools._")["items"]
    assert not query(compiler, prefix + "tools.unknown")["items"]
    assert not query(compiler, prefix + '# tools.')["items"]
    assert not query(compiler, prefix + '"tools."')["items"]

    # Function-wide and enclosing-scope bindings take precedence over imports.
    for source in (
        "# aether: 3.0\nimport m as tools\ndef main(tools: str) -> None:\n    tools.",
        prefix + 'tools.\n    tools = "local"\n',
        prefix + 'tools = "local"\n    tools.',
        prefix + 'tools = "local"\n    def inner() -> None:\n        tools.',
        "# aether: 3.0\nimport m as tools\ntools: str = \"global\"\ndef main() -> None:\n    tools.",
    ):
        caret = source.index("tools.") + len("tools.")
        answer = query(compiler, source, offset=caret)
        assert answer["handled"] and not answer["items"], answer
    # Checking a complete receiver rewrites a module-level binding to
    # AN_GLOBAL. Its original import spelling must still suppress guessed
    # exports when that binding shadows the module alias.
    shadowed = ('# aether: 3.0\nimport m as tools\ntools: str = "global"\n'
                'def main() -> None:\n    tools.value\n')
    answer = query(compiler, shadowed, offset=len(shadowed) - 1)
    assert answer["handled"] and not answer["items"], answer

    nested = '# aether: 3.0\ndef leaf() -> bool:\n    return true\n'
    answer = query(compiler, "# aether: 3.0\nimport nested.module as tools\ndef main() -> None:\n    tools.",
                   extra=[("/project/nested/module.as", nested)])
    assert [x["name"] for x in answer["items"]] == ["leaf"], answer
    # A dependency's public declarations are not implicitly re-exported.
    indirect = "# aether: 3.0\nfrom other import leaf\n" + MODULE.split("\n", 1)[1]
    answer = query(compiler, prefix + "tools.", indirect, [("/project/other.as", nested)])
    assert {x["name"] for x in answer["items"]} == expected, answer
    assert not query(compiler, prefix + "tools.", "# aether: 2\nx = 1\n")["items"]

    chinese = prefix + '# 中文\n    tools.pu'
    answer = query(compiler, chinese)
    assert answer["replace_start"] == len(chinese.encode()) - 2
    long_name = "public_" + "x" * 56
    answer = query(compiler, prefix + "tools.", f"# aether: 3.0\ndef {long_name}() -> None:\n    pass\n")
    assert [x["name"] for x in answer["items"]] == [long_name], answer
    many = "# aether: 3.0\n" + "".join(f"value_{n}: i64 = {n}\n" for n in range(100))
    answer = query(compiler, prefix + "tools.", many)
    assert len(answer["items"]) == 64 and answer["truncated"], answer

    # The completed source is checked by the same frontend. Member metadata
    # must point at exactly the declarations in the ordinary check report.
    completed = prefix + "tools.public()\n"
    result = subprocess.run([str(compiler), "check", "/project/main.as", "--snapshot-stdin", "--json"],
                             input=encode([("/project/main.as", completed), ("/project/m.as", MODULE)]),
                             capture_output=True, timeout=15)
    checked = json.loads(result.stdout)
    assert result.returncode == 0, checked
    symbols = {item["name"]: item for item in checked["symbols"] if item["path"] == "/project/m.as"}
    for item in report["items"]:
        assert item["start"] == symbols[item["name"]]["start"], (item, symbols)
    assert report["sources"][1] == checked["sources"][1]

    for offset in ("-1", "bad", "9999999999999999999", "999999"):
        result = subprocess.run([str(compiler), "complete", "/project/main.as", "--snapshot-stdin",
                                 "--at", offset], input=encode([("/project/main.as", chinese)]),
                                 capture_output=True, timeout=15)
        assert result.returncode == 2 and not result.stdout, result
    # UTF-8 byte offsets cannot bisect a character.
    cut = chinese.encode().index("中".encode()) + 1
    result = subprocess.run([str(compiler), "complete", "/project/main.as", "--snapshot-stdin",
                             "--at", str(cut)], input=encode([("/project/main.as", chinese)]),
                             capture_output=True, timeout=15)
    assert result.returncode == 2 and not result.stdout, result
    print("PASS A3 semantic completion: aliases, privacy, shadowing, source identities, UTF-8 and bounds")


def negative_control():
    """An accidental public/private boundary regression must turn the gate red."""
    original = ROOT / "c/apps/as/sema/completion.c"
    source = original.read_text()
    guard = "name[0] == '_' || "
    assert source.count(guard) == 1, "completion privacy guard changed"
    with tempfile.TemporaryDirectory(prefix="as-completion-control-") as directory:
        directory = Path(directory)
        mutant = directory / "completion.c"
        mutant.write_text(source.replace(guard, ""))
        compiler = directory / "asc"
        sources = [mutant if path == original else path for path in compiler_sources()]
        subprocess.run([os.environ.get("CC", "clang"), "-O1", "-g",
                        "-fsanitize=address,undefined", "-I" + str(original.parent),
                        "-I" + str(ROOT / "c/apps/as"),
                        "-I" + str(ROOT / "include/abi"), *map(str, sources),
                        "-o", str(compiler)], cwd=ROOT, check=True, capture_output=True)
        try:
            exercise(compiler)
        except AssertionError as error:
            assert error.args[0][0] == "module exports", error
            names = {item["name"] for item in error.args[0][1]["items"]}
            assert "_hidden" in names and "_private_value" in names, error
            print("PASS negative control: private module exports observed failing")
        else:
            raise AssertionError("Completion gate accepted private module exports")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()
    exercise(args.compiler.resolve())
    if args.negative:
        negative_control()
