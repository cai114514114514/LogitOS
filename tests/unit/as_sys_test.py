#!/usr/bin/env python3
"""Native sys library: typed callers, deterministic waits and owned argv.

Host transport copies are test-only; real file/process effects run in the guest.
The child-path probe inspects the actual argv passed to execve and then exits,
so a success marker alone cannot conceal argument corruption or continued child
execution after exec failure.
"""

import argparse
import json
from pathlib import Path
import shutil
import tempfile

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_abi_test import probe_runtime
from as_abi_wait_test import trace_probe

LIBRARY = ROOT / "fsroot/as/lib"
FIXTURE = ROOT / "tests/fixtures/astyped/sys-library/main.as"

WAIT_SOURCE = '''# aether: 3.0
import std.sys as system
def main() -> None:
    assert system.dns("local.test") == 42
    assert system.ping(42) == 17
    assert system.dns_or("local.test", 99) == 99
    assert system.ping_or(42, 88) == 88
    system.sleep(1)
    caught = false
    try:
        system.sleep(1)
    except IOError:
        caught = true
    assert caught
    assert system.run("missing", ["program"]) == -1
    print("native sys polling ok")
'''

# DNS fails immediately; ping remains pending for the entire five-second
# deadline. A failed fork must not issue waitpid(-1), which could reap any child.
WAIT_TRACE = [
    ("SYS_NET_DNS", 0), ("SYS_MONOTONIC_MS", 100), ("SYS_NET_DNS_RESULT", 42),
    ("SYS_NET_PING", 0), ("SYS_MONOTONIC_MS", 100), ("SYS_NET_PING_RTT", 17),
    ("SYS_NET_DNS", -1),
    ("SYS_NET_PING", 0), ("SYS_MONOTONIC_MS", 100), ("SYS_NET_PING_RTT", -1),
    ("SYS_MONOTONIC_MS", 5100),
    ("SYS_MONOTONIC_MS", 100), ("SYS_MONOTONIC_MS", 100), ("SYS_YIELD", 0),
    ("SYS_MONOTONIC_MS", 1100),
    ("SYS_MONOTONIC_MS", 100), ("SYS_MONOTONIC_MS", 99),
    ("SYS_FORK", -1),
]

ARGUMENT_SOURCE = '''# aether: 3.0
import std.sys as system
def main() -> None:
    values = ["program", "", "two words", ("prefix中文suffix").slice(6, 12)]
    system.spawn("/bin/program", values)
    raise AssertionError("failed exec continued in child")
'''

ARGUMENT_PROBE = '''#include "abi/logit_abi.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned stage;

int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    if (stage == 0) {
        assert(number == SYS_FORK);
        stage++;
        return 0;
    }
    if (stage == 1) {
        assert(number == SYS_EXECVE);
        assert(!strcmp((const char *)(uintptr_t)a, "/bin/program"));
        const char *const *arguments = (const char *const *)(uintptr_t)b;
        const char *expected[] = {"program", "", "two words", "中文"};
        for (unsigned i = 0; i < 4; i++) {
            assert(arguments[i] && !strcmp(arguments[i], expected[i]));
        }
        assert(arguments[4] == NULL && c == 0);
        stage++;
        return -1;
    }
    assert(stage == 2 && number == SYS_EXIT && a == 127);
    exit(127);
}
'''


def build_with_library(compiler, source, ir, library):
    result = invoke([compiler, "build", source, "--stdlib", library, "--emit-llvm", "-o", ir])
    assert result.returncode == 0, result.stdout + result.stderr


def argument_runtime(work, typed_failure=False):
    runtime = probe_runtime(work, ARGUMENT_PROBE)
    heap = runtime / "heap.c"
    text = heap.read_text()
    anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
    assert text.count(anchor) == 1
    heap.write_text(text.replace(anchor, anchor + "\n    at_gc_collect();"))
    if typed_failure:
        # Keep inspecting the actual arguments, then fail the language-level
        # transport instead of returning a negative kernel word. The child
        # still owes its caller an exit, not a caught exception and continuation.
        system = runtime / "system.c"
        text = system.read_text()
        anchor = "*out = abi_probe(number, a, b, c);"
        assert text.count(anchor) == 1
        system.write_text('#include "abi/logit_abi.h"\n' + text.replace(
            anchor, anchor + "\n    if (number == SYS_EXECVE) return AT_E_IO;"))
    return runtime


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-native-sys-") as temporary:
        work = Path(temporary)
        source = work / "invalid.as"
        for body in ('system.write_file("path", "text")',
                     'system.spawn("path", [1])',
                     'value: str = system.read_file("path")'):
            source.write_text("# aether: 3.0\nimport std.sys as system\n"
                              "def main() -> None:\n    " + body + "\n")
            result = invoke([compiler, "check", source, "--stdlib", LIBRARY, "--json"])
            report = json.loads(result.stdout)
            assert result.returncode == 1 and report["diagnostics"], report

        ir = work / "program.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "arguments", optimization)
            assert result.returncode == 0 and result.stdout == "native sys argument checks ok\n", result

        for name, text, probe, expected in (
            ("polling", WAIT_SOURCE, trace_probe(WAIT_TRACE), 0),
            ("argv", ARGUMENT_SOURCE, ARGUMENT_PROBE, 127),
            ("argv-error", ARGUMENT_SOURCE, ARGUMENT_PROBE, 127),
        ):
            directory = work / name
            directory.mkdir()
            source = directory / "main.as"
            source.write_text(text)
            runtime = (argument_runtime(directory, name == "argv-error")
                       if name.startswith("argv") else probe_runtime(directory, probe))
            emit_ir(compiler, source, ir)
            for optimization in ("-O0", "-O2"):
                result = sanitized(ir, runtime, directory / "program", optimization)
                assert result.returncode == expected and not result.stderr, result
                assert result.stdout == ("native sys polling ok\n" if name == "polling" else ""), result
    print("PASS native sys: typed callers, argument validation, polling, owned argv and exec failure, O0/O2")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-sys-controls-") as temporary:
        work = Path(temporary)
        library = work / "lib"
        shutil.copytree(LIBRARY, library)
        module = library / "sys.as"
        original = module.read_text()
        cases = [
            ("argv", ARGUMENT_SOURCE, 'value = Bytes(argument + chr(0))',
             'value = Bytes(argument + "suffix" + chr(0))', 127),
            ("wait-any", WAIT_SOURCE, "if child <= 0:", "if child == 0:", 0),
            ("argv-error", ARGUMENT_SOURCE, "            pass\n        proc_exit(127)",
             "            raise\n        proc_exit(127)", 127),
        ]
        for name, text, anchor, replacement, status in cases:
            directory = work / name
            directory.mkdir()
            source = directory / "main.as"
            source.write_text(text)
            runtime = (argument_runtime(directory, name == "argv-error")
                       if name.startswith("argv") else probe_runtime(directory, trace_probe(WAIT_TRACE)))
            ir = directory / "main.ll"
            module.write_text(original)
            build_with_library(compiler, source, ir, library)
            result = sanitized(ir, runtime, directory / "positive", "-O0")
            assert result.returncode == status and not result.stderr, result
            assert original.count(anchor) == 1
            module.write_text(original.replace(anchor, replacement))
            build_with_library(compiler, source, ir, library)
            result = sanitized(ir, runtime, directory / "negative", "-O0")
            expected_error = "IOError" if name == "argv-error" else "assert"
            assert result.returncode != status and expected_error.lower() in result.stderr.lower(), result
            print("PASS sys control:", name, "observed failing")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    (negative_controls if args.negative_control else exercise)(args.compiler.resolve())
