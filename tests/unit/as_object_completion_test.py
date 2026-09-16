#!/usr/bin/env python3
"""Object completions come from checked types, including incomplete receivers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from as_compiler import ROOT, compiler_sources
from as_semantic_completion_test import query

BOX = '''# aether: 3.0
class Box:
    value: i64
    _data: i64
    def read(self) -> i64:
        return self.value
    def _secret(self) -> i64:
        return self._data
'''


def names(report):
    assert report["handled"], report
    return {item["name"] for item in report["items"]}


def exercise(compiler):
    expected = {"value", "_data", "read"}
    parameter = BOX + "def main(box: Box) -> None:\n    box."
    answer = query(compiler, parameter)
    assert names(answer) == expected, ("object members", answer)
    assert names(query(compiler, parameter + "va")) == {"value"}
    assert names(query(compiler, parameter + "read")) == {"read"}
    assert names(query(compiler, parameter + "unknown")) == set()
    assert names(query(compiler, parameter + "_secret")) == set()

    for receiver in ("box.", "Box(7, 8).", "identity(box).", "boxes[0]."):
        source = BOX + '''def identity[T](value: T) -> T:
    return value
def main() -> None:
    box = Box(7, 8)
    boxes = [box]
    ''' + receiver
        assert names(query(compiler, source)) == expected, receiver

    # The innermost checked receiver carries both the field's inferred type
    # and the original dot; no string parsing of `holder.box` is necessary.
    chained = BOX + '''struct Holder:
    box: Box
def main(holder: Holder) -> None:
    holder.box.'''
    assert names(query(compiler, chained)) == expected

    private = BOX + '''    def inspect(self) -> None:
        def nested() -> None:
            self.'''
    assert names(query(compiler, private)) == expected | {"_secret", "inspect"}

    # Field tokens must keep the base module's path. Overrides must appear
    # once and refer to the derived method's actual declaration.
    derived = '''# aether: 3.0
from m import Box
class Child(Box):
    other: bool
    def read(self) -> i64:
        return 9
def main(child: Child) -> None:
    child.'''
    answer = query(compiler, derived, BOX)
    assert names(answer) == expected | {"other"}, answer
    assert len(answer["items"]) == 4, answer
    by_name = {item["name"]: item for item in answer["items"]}
    assert by_name["value"]["path"] == "/project/m.as", answer
    assert by_name["read"]["path"] == "/project/main.as", answer
    assert by_name["other"]["type"] == "bool", answer
    super_source = derived.split("def main", 1)[0] + "    def inspect(self) -> None:\n        super."
    assert names(query(compiler, super_source, BOX)) == {"read"}
    formatted = BOX + 'def main(box: Box) -> None:\n    print(f"{box.va}")\n'
    caret = formatted.index("box.va") + len("box.va")
    assert names(query(compiler, formatted, offset=caret)) == {"value"}

    # Query only the selected receiver even while the remaining body is being
    # typed. Running each prefix under ASan/UBSan exercises recovery ownership.
    fragments = ["if true:\n        box.", "box.\n    return", "box.\n    value = (",
                 "box.\n    other = ["]
    for fragment in fragments:
        source = BOX + "def main(box: Box) -> None:\n    " + fragment
        caret = source.index("box.") + len("box.")
        assert names(query(compiler, source, offset=caret)) == expected, fragment
    # A later unfinished string must no longer erase this receiver's class.
    source = parameter + '\n    text = "unfinished'
    assert names(query(compiler, source, offset=len(parameter))) == expected

    # A counterfactual class with identical variable spelling must change the
    # candidates. This catches the old last-assignment/name guessing approach.
    different = "# aether: 3.0\nstruct Other:\n    different: f64\n"
    assert names(query(compiler, different + "def main(box: Other) -> None:\n    box.")) == {"different"}
    # Completion asks the checker to visit recovered bodies. Exercise every
    # ordinary typing prefix, rather than testing only completed declarations.
    for end in range(len("# aether: 3.0\n"), len(chained) + 1):
        query(compiler, chained[:end])
    print("PASS A3 object completion: typed receivers, fields, methods, inheritance, privacy and recovery")


def negative_control():
    original = ROOT / "c/apps/as/sema/completion_object.c"
    source = original.read_text()
    guard = "lexical_owner != method->method_owner"
    assert source.count(guard) == 1, "method visibility guard changed"
    with tempfile.TemporaryDirectory(prefix="as-object-completion-") as temporary:
        directory = Path(temporary)
        mutant = directory / original.name
        mutant.write_text(source.replace(guard, "0"))
        compiler = directory / "asc"
        sources = [mutant if path == original else path for path in compiler_sources()]
        subprocess.run([os.environ.get("CC", "clang"), "-O1", "-g",
                        "-fsanitize=address,undefined", "-I" + str(original.parent),
                        "-I" + str(ROOT / "c/apps/as"), "-I" + str(ROOT / "include/abi"),
                        *map(str, sources), "-o", str(compiler)], cwd=ROOT, check=True,
                       capture_output=True)
        try:
            exercise(compiler)
        except AssertionError as error:
            assert error.args[0][0] == "object members", error
            assert "_secret" in names(error.args[0][1]), error
            print("PASS negative control: private method completion observed failing")
        else:
            raise AssertionError("Object completion gate accepted inaccessible methods")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()
    exercise(args.compiler.resolve())
    if args.negative:
        negative_control()
