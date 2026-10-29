#!/usr/bin/env python3
"""Native diagnostics retain surrounding declarations without accepting bad code."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from as_snapshot_test import encode
from as_semantic_completion_test import query

SOURCE = '''# aether: 3.0
def before() -> i64:
    return 1
def broken() -> None:
    text = "中文未完成
def after() -> i64:
    return 7
'''


def check(compiler, source, modules=()):
    result = subprocess.run([str(compiler), "check", "/project/main.as", "--snapshot-stdin", "--json"],
                            input=encode([("/project/main.as", source), *modules]),
                            capture_output=True, timeout=15)
    assert result.returncode == 1 and not result.stderr, result
    report = json.loads(result.stdout)
    assert not report["ok"], report
    return report


def exercise(compiler):
    answer = check(compiler, SOURCE)
    assert {item["name"] for item in answer["symbols"]} == {"before", "broken", "after"}, answer
    assert len(answer["diagnostics"]) == 1, answer
    error = answer["diagnostics"][0]
    assert error["code"] == "AS1001" and error["line"] == 5 and error["column"] == 12, error
    assert SOURCE.encode()[error["start"]:error["end"]].decode() == '"中文未完成', error
    assert error["end_column"] == 18, error
    for fragment in ('"unfinished', "'unfinished", 'f"{1', "0x", "@", "!"):
        source = SOURCE.replace('"中文未完成', fragment)
        answer = check(compiler, source)
        assert len(answer["diagnostics"]) == 1, (fragment, answer)
        assert {item["name"] for item in answer["symbols"]} == {"before", "broken", "after"}, answer
    source = SOURCE.replace('text = "中文未完成', "变量 = 1")
    answer = check(compiler, source)
    assert len(answer["diagnostics"]) == 1, answer
    error = answer["diagnostics"][0]
    assert error["column"] == 5 and error["end_column"] == 11, error
    assert source.encode()[error["start"]:error["end"]].decode() == "变量 = 1", error

    source = "# aether: 3.0\n" + "".join(f"broken_{i} = @\n" for i in range(40))
    answer = check(compiler, source)
    assert answer["truncated"] and len(answer["diagnostics"]) == 32, answer
    assert all(item["code"] == "AS1001" for item in answer["diagnostics"]), answer

    # Both a prior and a following declaration remain available through the
    # same immutable imported snapshot, despite its lexical diagnostic.
    source = "# aether: 3.0\nimport m\ndef main() -> None:\n    m."
    answer = query(compiler, source, SOURCE)
    assert {item["name"] for item in answer["items"]} == {"before", "broken", "after"}, answer

    with tempfile.TemporaryDirectory(prefix="as-recovery-driver-") as temporary:
        directory = Path(temporary)
        marker = directory / "ran"
        source = ("# aether: 3.0\ndef main() -> None:\n"
                  f'    file_write("{marker}", Bytes("ran"))\n'
                  "    broken = 'unfinished\n")
        path = directory / "main.as"
        output = directory / "keep.ll"
        path.write_text(source)
        output.write_bytes(b"previous artifact")
        for command in ("check", "build", "run", "test"):
            args = [str(compiler), command, str(path), "--json", "--snapshot-stdin"]
            if command == "build":
                args += ["--emit-llvm", "-o", str(output)]
            result = subprocess.run(args, input=encode([(str(path), source)]),
                                    capture_output=True, timeout=15)
            report = json.loads(result.stdout)
            assert result.returncode == 1 and len(report["diagnostics"]) == 1, result
            assert not marker.exists() and output.read_bytes() == b"previous artifact", command

    print("PASS A3 recovery: UTF-8 ranges, later declarations, bounded errors and strict build/run/test")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    exercise(parser.parse_args().compiler.resolve())
