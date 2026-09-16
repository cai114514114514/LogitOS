#!/usr/bin/env python3
"""Exercise the shipped native shell, including exact script and prompt output.

The old host shell gate still runs the coreutils round-trip. This gate adds
argument boundaries, parse-error recovery and observable redirection contents.
The guest runner consumes the same transcript and adds OS-specific cd/pwd.
"""

import argparse
from pathlib import Path
import tempfile

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_command_test import child_program

SOURCE = ROOT / "fsroot/as/examples/ash.as"
BANNER = "ash: LogitOS AetherScript shell\n"


def transcript(directory, child, guest=False):
    # Each expected response is fixed independently of the shell. Keeping the
    # empty argument between quotes catches tokenizers that drop it silently.
    lines = [
        ('echo "quoted 中文" | cat', "quoted 中文\n"),
        (f'echo persisted > "{directory}/ash-output"', ""),
        (f'cat < "{directory}/ash-output"', "persisted\n"),
        (f'{child} arguments "" "two words" 中文', "arguments ok\n"),
        ("| cat", "ash: syntax error near '|'\n"),
        ("echo recovered", "recovered\n"),
    ]
    if guest:
        lines.extend([(f"cd {directory}", ""), ("pwd", str(directory) + "\n")])
    lines.append(("exit", "ash: bye\n"))
    script = "\n".join(command for command, _ in lines) + "\necho UNREACHABLE\n"
    output = BANNER + "".join(reply for _, reply in lines)
    interactive = BANNER + "".join("ash$ " + reply for _, reply in lines)
    return script, output, interactive


def require_script(result, expected, directory):
    assert result.returncode == 0 and result.stdout == expected, result
    assert (directory / "ash-output").read_text() == "persisted\n"


def exercise(compiler, negative_control=False):
    with tempfile.TemporaryDirectory(prefix="as-native-shell-") as temporary:
        work = Path(temporary)
        child = child_program(compiler, work)
        script, expected, interactive = transcript(work, child)
        path = work / "commands.txt"
        path.write_text(script)
        ir = work / "ash.ll"
        emit_ir(compiler, SOURCE, ir)
        for optimization in (("-O0",) if negative_control else ("-O0", "-O2")):
            binary = work / "ash"
            result = sanitized(ir, RUNTIME, binary, optimization, (path,))
            require_script(result, expected, work)
            result = invoke([binary], input=script)
            assert result.returncode == 0 and result.stdout == interactive, result
            result = invoke([binary], input="echo EOF\n")
            assert result.returncode == 0 and result.stdout == (
                BANNER + "ash$ EOF\nash$ ash: eof\n"), result
            missing = work / "missing-script"
            result = invoke([binary, missing])
            assert result.returncode == 1 and result.stdout == (
                BANNER + f"ash: cannot open {missing}\n"), result

        if negative_control:
            # Remove a real redirect in a private copy, then require the
            # transcript oracle to fail despite the unchanged exit banner.
            source = work / "ash.as"
            text = SOURCE.read_text()
            anchor = "cmd = cmd -> outfile"
            assert text.count(anchor) == 1
            source.write_text(text.replace(anchor, "pass"))
            (work / "ash-output").unlink()
            emit_ir(compiler, source, ir)
            result = sanitized(ir, RUNTIME, work / "broken", "-O0", (path,))
            assert "ash: bye\n" in result.stdout, result
            try:
                require_script(result, expected, work)
            except AssertionError:
                print("PASS shell control: lost redirect rejected despite clean exit")
            else:
                raise AssertionError("Shell oracle accepted missing redirection")
        else:
            print("PASS native ash: script, interactive, EOF, errors, argv and files O0/O2")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    exercise(args.compiler.resolve(), args.negative_control)
