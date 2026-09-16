#!/usr/bin/env python3
"""Validate the actual A3 media/chat launchers and their ABI order.

The host probe replaces only syscall transport. Actual Preview decoding is
verified by the separate guest association test on the same native artifacts.
"""

import argparse
import json
from pathlib import Path
import shutil
import tempfile

from as_abi_test import probe_runtime
from as_managed_test import ROOT, emit_ir, sanitized

FIXTURES = ROOT / "tests/fixtures/preview"
MEDIA = {
    "open-audio": "/media/sample.mp3",
    "open-flac": "/media/sample.flac",
    "open-wav": "/media/sample.wav",
    "open-mp4": "/media/clip.mp4",
    "open-mkv": "/media/clip.mkv",
    "open-webm": "/media/clip.webm",
    "open-image": "/media/img/still.webp",
}


def probe(path, failure=None):
    create_result = -1 if failure == "create" else 0
    open_result = -1 if failure == "open" else 0
    expected_calls = 1 if failure == "create" else 2
    return '''#include "abi/logit_abi.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static unsigned calls;
int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    if (calls++ == 0) {
        assert(number == SYS_GUI_CREATE);
        assert(!strcmp((const char *)(uintptr_t)a, "opener"));
        assert(b == ((uint64_t)200 << 16 | 100) && c == 0);
        return CREATE_RESULT;
    }
    assert(calls == 2 && number == SYS_OPEN_PATH);
    assert(!strcmp((const char *)(uintptr_t)a, MEDIA_PATH));
    assert(b == 0 && c == 0);
    return OPEN_RESULT;
}

__attribute__((destructor)) static void check_calls(void)
{
    assert(calls == EXPECTED_CALLS);
}
'''.replace("CREATE_RESULT", str(create_result)).replace("OPEN_RESULT", str(open_result)).replace(
        "MEDIA_PATH", json.dumps(path)).replace("EXPECTED_CALLS", str(expected_calls))


def host_checks(compiler, negative):
    assert {path.stem for path in FIXTURES.glob("open-*.as")} == set(MEDIA)
    with tempfile.TemporaryDirectory(prefix="as-preview-launch-") as temporary:
        work = Path(temporary)
        for name, path in MEDIA.items():
            directory = work / name
            directory.mkdir()
            runtime = probe_runtime(directory, probe(path))
            ir = directory / "program.ll"
            emit_ir(compiler, FIXTURES / (name + ".as"), ir)
            for mode in ("-O0", "-O2"):
                result = sanitized(ir, runtime, directory / "program", mode)
                assert result.returncode == 0 and result.stdout == "opening " + path + "\n", result

        # The common helper must stop if window creation fails, and must not
        # treat a refused association call as a successful process exit.
        for failure in ("create", "open"):
            directory = work / failure
            directory.mkdir()
            runtime = probe_runtime(directory, probe(MEDIA["open-image"], failure))
            for mode in ("-O0", "-O2"):
                result = sanitized(work / "open-image/program.ll", runtime, directory / "program", mode)
                assert result.returncode == 1 and "IOError" in result.stderr, result
                if failure == "create":
                    assert not result.stdout, result
                else:
                    assert MEDIA["open-image"] in result.stderr, result

        # Chat uses the same GUI launch boundary, but retains its original
        # public success/failure strings for the existing interactive gate.
        for failure in (None, "create", "open"):
            directory = work / ("chat-" + str(failure))
            directory.mkdir()
            runtime = probe_runtime(directory, probe("/bin/ch.aex", failure))
            ir = directory / "program.ll"
            emit_ir(compiler, ROOT / "fsroot/as/examples/chlaunch.as", ir)
            for mode in ("-O0", "-O2"):
                result = sanitized(ir, runtime, directory / "program", mode)
                assert result.returncode == (1 if failure else 0), result
                if failure == "create":
                    assert not result.stdout and "IOError" in result.stderr, result
                else:
                    expected = "CHLAUNCH_FAILED rc=-1\n" if failure else "CHLAUNCH_OK\n"
                    assert result.stdout == expected, result

        if negative:
            directory = work / "no-window"
            directory.mkdir()
            source = directory / "open-image.as"
            shutil.copyfile(FIXTURES / source.name, source)
            helper = (FIXTURES / "association.as").read_text()
            anchor = 'if gui_create("opener", 200, 100) < 0:'
            assert helper.count(anchor) == 1
            (directory / "association.as").write_text(helper.replace(anchor, "if false:"))
            ir = directory / "program.ll"
            emit_ir(compiler, source, ir)
            result = sanitized(ir, work / "open-image/runtime", directory / "program", "-O0")
            assert result.returncode != 0 and "assert" in result.stderr.lower(), result
            print("PASS association control: omitted window creation fails the ABI call-order assertion")
    print("PASS seven media launchers and Chat: exact paths, GUI ownership, failures, O0/O2")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    host_checks(args.compiler.resolve(), args.negative_control)
