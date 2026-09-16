#!/usr/bin/env python3
"""Native commands: real children, pipelines, ownership and failure controls."""
import argparse
import json
from pathlib import Path
import re
import tempfile

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_runtime import copy_runtime

FIXTURE = ROOT / "tests/fixtures/astyped/command/main.as"


def diagnostics(compiler, work):
    cases = {
        "empty-call": ("run()", "AS3204"),
        "argument-type": ("run(7)", "AS3202"),
        "list-type": ("run([1, 2])", "AS3202"),
        "method-arity": ('run("echo").wait(1)', "AS3204"),
        "pipeline-type": ('run("echo") |> 3', "AS3202"),
        "redirect-type": ('run("echo") -> 3', "AS3202"),
        "unscoped-start": ('process = run("echo").start()', "AS3401"),
        "copy-owner": ('with process = run("echo"):\n    alias = process', "AS3401"),
        "owner-rebind": ('with process = run("echo"):\n    process = 2', "AS3401"),
        "owner-box": ('with process = run("echo"):\n    alias: Any = process', "AS3401"),
        "owner-expired": ('with process = run("echo"):\n    pass\nprocess.wait()', "AS3205"),
        "owner-capture": ('with process = run("echo"):\n    def saved() -> i64:\n'
                          '        return process.wait()\n    saved()', "AS3401"),
    }
    for name, (body, code) in cases.items():
        source = work / (name + ".as")
        source.write_text("# aether: 3.0\ndef main() -> None:\n" +
                          "\n".join("    " + line for line in body.splitlines()) + "\n")
        checked = invoke([compiler, "check", source, "--json"])
        assert checked.stdout, (name, checked)
        report = json.loads(checked.stdout)
        assert checked.returncode == 1 and any(
            error["code"] == code for error in report["diagnostics"]), (name, report)


def child_program(compiler, work):
    child = work / "command-child"
    result = invoke([compiler, "build", FIXTURE.with_name("child.as"),
                     "--toolchain", RUNTIME, "-o", child])
    assert result.returncode == 0, result
    return child


def require_commands(result):
    assert result.returncode == 0 and result.stdout == "native commands ok\n", result


def lifecycle_controls(work, negative_control=False):
    runtime = work / "lifecycle-runtime"
    copy_runtime(runtime)
    launch = runtime / "command_launch.c"
    launch.write_text("#define fork command_test_fork\n#define pipe command_test_pipe\n" +
                      launch.read_text())
    command = runtime / "command.c"
    command.write_text("#define read command_test_read\n#define realloc command_test_realloc\n"
                       "#define pipe command_test_pipe\n" + command.read_text())
    source = ROOT / "tests/unit/as_command_runtime_test.c"
    result = sanitized(source, runtime, work / "lifecycle", "-O0")
    assert result.returncode == 0 and result.stdout.endswith("native command lifecycle ok\n"), result
    print("PASS command lifecycle: partial launch/read/allocation failure, closed stdio, SIGPIPE, fd 4096")
    if negative_control:
        original = launch.read_text()
        anchor = "at_command_abort(head);"
        assert original.count(anchor) == 1
        launch.write_text(original.replace(anchor, "/* omitted partial-launch cleanup */"))
        result = sanitized(source, runtime, work / "lost-child-cleanup", "-O0")
        assert result.returncode != 0 and "command left an unreaped child" in result.stderr, result
        print("PASS command control: partial-launch child leak rejected and cleaned by harness")


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-commands-") as temporary:
        work = Path(temporary)
        lifecycle_controls(work)
        diagnostics(compiler, work)
        child = child_program(compiler, work)
        ir = work / "commands.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            require_commands(sanitized(ir, RUNTIME, work / "commands", optimization, (work, child)))
        emit_ir(compiler, FIXTURE.with_name("errors.as"), ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "errors", optimization, (work,))
            assert result.returncode == 0 and result.stdout == (
                "redirected\nnative command errors ok\n"), result
    print("PASS commands: 12 diagnostics, real argv/pipes/redirection/status/scoped children, O0/O2")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-command-controls-") as temporary:
        work = Path(temporary)
        lifecycle_controls(work, negative_control=True)
        child = child_program(compiler, work)
        ir = work / "commands.ll"
        emit_ir(compiler, FIXTURE, ir)
        copy_runtime(work)
        launch = work / "command_launch.c"
        original = launch.read_text()
        require_commands(sanitized(ir, work, work / "positive", "-O0", (work, child)))
        # Lost stdout wiring still launches both real children, so only the
        # actual captured bytes (not a PID or a final marker) reject it.
        anchor = "dup2(write_end, STDOUT_FILENO) < 0"
        assert original.count(anchor) == 1
        launch.write_text(original.replace(anchor, "0"))
        result = sanitized(ir, work, work / "lost-output", "-O0", (work, child))
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        print("PASS command control: missing output wiring loses actual child bytes")
        launch.write_text(original)

        original_ir = ir.read_text()
        changed, count = re.subn(r"call i32 @at_command_release\(ptr [^\n]+\)",
                                "add i32 0, 0", original_ir)
        assert count > 0, "process cleanup anchor changed"
        ir.write_text(changed)
        result = sanitized(ir, work, work / "lost-wait", "-O0", (work, child))
        assert result.returncode == 1 and "AssertionError" in result.stderr, result
        print("PASS command control: missing scoped wait leaves status unavailable at scope exit")
        ir.write_text(original_ir)

        # Force collection while command descriptions, argv and pipeline links
        # are retained. The positive run must survive each allocation first.
        heap = work / "heap.c"
        source = heap.read_text()
        anchor = "if (live_bytes + bytes >= collect_at)"
        assert source.count(anchor) == 1
        heap.write_text(source.replace(anchor, "if (1)"))
        require_commands(sanitized(ir, work, work / "collect-positive", "-O0", (work, child)))
        command = work / "command.c"
        source = command.read_text()
        anchor = "at_gc_mark(command->arguments);"
        assert source.count(anchor) == 1
        command.write_text(source.replace(anchor, "/* missing argv owner */"))
        result = sanitized(ir, work, work / "lost-argv", "-O0", (work, child))
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result
        print("PASS command control: missing command argv root fails under ASan")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls(args.compiler.resolve())
    else:
        exercise(args.compiler.resolve())
