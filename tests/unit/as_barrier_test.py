#!/usr/bin/env python3
"""Check the native barrier probe, its parsing failures and actual guest writes.

Host transport responses test parsing and arithmetic only. The guest check
requires the real device's writeback-cache declaration and kernel counters;
neither result proves physical hardware durability or crash recovery.
"""

import argparse
import json
from pathlib import Path
import re
import shlex
import tempfile

from as_abi_test import probe_runtime
from as_managed_test import ROOT, emit_ir, sanitized

SOURCE = ROOT / "fsroot/as/examples/barriers.as"
CONTENT = "a barrier probe, written to force a transaction"


def check_output(output):
    match = re.fullmatch(r"BARRIERS (\d+) -> (\d+) delta (\d+)\nBARRIERS-OK\n", output)
    if not match:
        raise AssertionError(f"incomplete barrier measurement: {output!r}")
    before, after, delta = map(int, match.groups())
    if after - before != delta or delta < 3:
        raise AssertionError(f"invalid barrier difference: {output!r}")
    return {"before": before, "after": after, "delta": delta}


def check_file(path):
    if path.read_text() != CONTENT:
        raise AssertionError("barrier probe did not write the complete expected file")


def run_guest_barriers(guest, executable, program):
    if b"cache=writeback (barriers REQUIRED)" not in bytes(guest.log):
        raise AssertionError("guest device did not advertise the writeback cache under test")
    output = guest.capture("/bin/native-capture " + shlex.quote(executable), timeout=45)
    assert guest.last_capture_exit == 0, output
    measurement = check_output(output)
    contents = guest.capture("/bin/cat /dur/barrier.probe", timeout=30)
    assert guest.last_capture_exit == 0 and contents == CONTENT, contents
    program.update(exit_code=0, output=output, barrier_measurement=measurement,
                   file_contents=contents, writeback_cache=True)


def transport(before, after, result_count="strlen(text)"):
    return '''#include "abi/logit_abi.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    static unsigned calls;
    assert(number == SYS_SYSINFO && b == 4096 && c == 0);
    const char *text = calls++ == 0 ? BEFORE : AFTER;
    memcpy((void *)(uintptr_t)a, text, strlen(text));
    return RESULT;
}
'''.replace("BEFORE", json.dumps(before)).replace("AFTER", json.dumps(after)).replace("RESULT", result_count)


def host_checks(compiler, negative):
    with tempfile.TemporaryDirectory(prefix="as-barriers-") as temporary:
        work = Path(temporary).resolve()
        path = work / "barrier.probe"
        source = work / "barriers.as"
        original = SOURCE.read_text().replace('"/dur/barrier.probe"', json.dumps(str(path)))
        source.write_text(original)
        ir = work / "barriers.ll"
        emit_ir(compiler, source, ir)

        # The host writes a real temporary file, but these counters are probe
        # responses. Only the separate guest gate establishes real barriers.
        cases = (
            ("normal", "Barriers 31\n", "Barriers 34\n", "strlen(text)", 0),
            ("frozen", "Barriers 31\n", "Barriers 31\n", "strlen(text)", 1),
            ("refused", "", "", "-1", 1),
            ("absent", "Other 31\n", "", "strlen(text)", 1),
            ("duplicate", "Barriers 31\nBarriers 32\n", "", "strlen(text)", 1),
            ("oversized", "Barriers 31\n", "", "4097", 1),
            ("after-absent", "Barriers 31\n", "Other 34\n", "strlen(text)", 1),
        )
        for label, before, after, count, status in cases:
            directory = work / label
            directory.mkdir()
            runtime = probe_runtime(directory, transport(before, after, count))
            for mode in ("-O0", "-O2"):
                path.unlink(missing_ok=True)
                result = sanitized(ir, runtime, directory / "probe", mode)
                assert result.returncode == status, (label, result)
                if status == 0:
                    assert check_output(result.stdout) == {"before": 31, "after": 34, "delta": 3}
                    check_file(path)
                else:
                    assert "BARRIERS-FAIL" in result.stdout and "BARRIERS-OK" not in result.stdout, result

        if negative:
            # Remove the actual write while retaining both queried counters
            # and the success marker. Readback must expose this lying probe.
            write = next(line for line in original.splitlines(True) if line.startswith("    file_write("))
            source.write_text(original.replace(write, ""))
            emit_ir(compiler, source, ir)
            path.unlink(missing_ok=True)
            result = sanitized(ir, work / "normal/runtime", work / "missing-write", "-O0")
            assert result.returncode == 0, result
            check_output(result.stdout)
            try:
                check_file(path)
            except (FileNotFoundError, AssertionError):
                pass
            else:
                raise AssertionError("the missing-write control passed file readback")
            print("PASS barrier control: removed write retained success output, file readback rejected it")
            for output in ("BARRIERS-OK\n", "BARRIERS 31 -> 31 delta 3\nBARRIERS-OK\n",
                           "BARRIERS 31 -> 32 delta 1\nBARRIERS-OK\n"):
                try:
                    check_output(output)
                except AssertionError:
                    pass
                else:
                    raise AssertionError("invalid barrier measurement passed")
            print("PASS barrier controls: marker-only, fabricated arithmetic and insufficient barriers rejected")
    print("PASS native barrier tool: counter parsing, I/O errors and real file writes, O0/O2")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    host_checks(args.compiler.resolve(), args.negative_control)
