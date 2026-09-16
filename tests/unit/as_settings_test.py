#!/usr/bin/env python3
"""Run the actual settings CLI natively; replace only the host syscall transport.

The probe checks the kernel ABI independently of the language's constant
manifest. File commands use real private files. Reboot persistence belongs to
the separate guest gate; a host transport response cannot establish it.
"""

import argparse
import json
import os
from pathlib import Path
import tempfile

from as_abi_test import probe_runtime
from as_managed_test import ROOT, emit_ir, invoke, sanitized

SOURCE = ROOT / "fsroot/as/examples/setcheck.as"
GARBAGE = (
    "# hand-edited, badly\nui.dark = banana\nui.accent = 0xFFFFFFFF\n"
    "desktop.restore_session = -1\nnet.dhcp = 2\nnet.ip = 999.1.1.1\n"
    "win.clock.frame = -9000 -9000 4 4 1 0 0 0 0 0 0\n"
    "= nokey\nno_equals_sign_at_all\nui.wallpaper = /does/not/exist.png\n"
).encode()

PROBE = r'''#include "abi/logit_abi.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned calls;

/* The test supplies expectations, not a replacement settings implementation.
 * In particular commit=1 is checked even though this host cannot commit the
 * guest store. Removing that flag must fail before any success is accepted. */
int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    calls++;
    const char *key = getenv("SETTING_KEY");
    const char *value = getenv("SETTING_VALUE");
    const char *result = getenv("SETTING_RESULT");
    if (number == SYS_SETTING_GET) {
        assert(key && !strcmp((const char *)(uintptr_t)a, key));
        assert(b && c == 256);
        if (value) {
            assert(strlen(value) < c);
            memcpy((void *)(uintptr_t)b, value, strlen(value) + 1);
        }
        return result ? strtoll(result, NULL, 10) : (int64_t)strlen(value);
    }
    if (number == SYS_SETTING_SET) {
        assert(key && !strcmp((const char *)(uintptr_t)a, key));
        assert(value && !strcmp((const char *)(uintptr_t)b, value));
        assert(c == 1);
        return result ? strtoll(result, NULL, 10) : 0;
    }
    assert(number == SYS_SETTING_CTL && b == 0 && c == 0);
    if (getenv("SETTING_DIAG")) {
        assert(a == (calls == 1 ? SETCTL_DIAG : SETCTL_KVCOUNT));
        return calls == 1 ? 3 : 9;
    }
    assert(getenv("SETTING_OPERATION"));
    assert(a == (uint64_t)strtoll(getenv("SETTING_OPERATION"), NULL, 10));
    return result ? strtoll(result, NULL, 10) : 0;
}

__attribute__((destructor)) static void check_calls(void)
{
    const char *expected = getenv("SETTING_CALLS");
    assert(calls == (expected ? (unsigned)strtoul(expected, NULL, 10) : 0));
}
'''


def run(binary, arguments, status, output, **expectations):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("SETTING_")}
    environment.update(ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1")
    environment.update({"SETTING_" + key.upper(): str(value)
                        for key, value in expectations.items()})
    result = invoke([binary, *arguments], env=environment)
    assert result.returncode == status, (arguments, result)
    if output is not None:
        assert result.stdout == output, (arguments, result)
    return result


def commands(binary, path):
    for command in ([], ["get"], ["set", "ui.dark"], ["check"], ["frame"],
                    ["truncate"], ["unknown"]):
        run(binary, command, 2, None)
    run(binary, ["get", "ui.dark"], 0, "SETCHECK-VALUE ui.dark = 1\n",
        key="ui.dark", value="1", calls=1)
    run(binary, ["get", "missing"], 0, "SETCHECK-VALUE missing = \n",
        key="missing", result=-1, calls=1)
    run(binary, ["check", "ui.dark", "1"], 0, "SETCHECK-OK ui.dark = 1\n",
        key="ui.dark", value="1", calls=1)
    run(binary, ["check", "ui.dark", "0"], 1,
        "SETCHECK-BAD ui.dark want 0 got 1\n", key="ui.dark", value="1", calls=1)
    for invalid in (-2, 256, 257):
        result = run(binary, ["get", "ui.dark"], 1, "", key="ui.dark",
                     value="1", result=invalid, calls=1)
        assert "IOError" in result.stderr, result
    for status, returned, text in ((0, 0, "SETCHECK-SET ui.dark = 1\n"),
                                   (1, -1, "SETCHECK-SET-FAIL ui.dark\n")):
        run(binary, ["set", "ui.dark", "1"], status, text, key="ui.dark",
            value="1", result=returned, calls=1)
    frame = "40 60 320 240 1 0 0 40 60 320 240"
    run(binary, ["frame", "clock", "40", "60", "320", "240"], 0,
        "SETCHECK-SET win.clock.frame = " + frame + "\n",
        key="win.clock.frame", value=frame, calls=1)
    for command, operation, output in (("reset", 2, "SETCHECK-RESET\n"),
                                        ("reload", 5, "SETCHECK-RELOAD 0\n"),
                                        ("selftest", 6, "SETCHECK-SELFTEST-OK\n")):
        run(binary, [command], 0, output, operation=operation, calls=1)
        rejected = run(binary, [command], 1, "", operation=operation, result=-1, calls=1)
        assert "IOError" in rejected.stderr, rejected
    run(binary, ["selftest"], 1, "SETCHECK-SELFTEST-FAIL 2\n",
        operation=6, result=2, calls=1)
    run(binary, ["diag"], 0, "SETCHECK-DIAG 3 keys 9\n", diag=1, calls=2)

    original = "prefix中文\n".encode()
    for length in (0, 8, 100):
        path.write_bytes(original)
        count = min(length, len(original))
        run(binary, ["truncate", str(length)], 0,
            f"SETCHECK-TRUNCATED {count} of {len(original)}\n")
        assert path.read_bytes() == original[:count], "truncation did not write the actual prefix"
    for invalid in ("-1", "bad"):
        path.write_bytes(original)
        run(binary, ["truncate", invalid], 1, "")
        assert path.read_bytes() == original, "invalid length changed the file"
    path.unlink()
    run(binary, ["truncate", "1"], 1, "SETCHECK-TRUNC-FAIL no file\n")
    run(binary, ["garbage"], 0, f"SETCHECK-GARBAGE-WRITTEN {len(GARBAGE)}\n")
    assert path.read_bytes() == GARBAGE, "malformed-file fixture changed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-settings-") as temporary:
        work = Path(temporary).resolve()
        path = work / "settings.conf"
        source = work / "setcheck.as"
        original = SOURCE.read_text().replace('"/etc/settings.conf"', json.dumps(str(path)))
        source.write_text(original)
        ir = work / "settings.ll"
        emit_ir(args.compiler.resolve(), source, ir)
        runtime = probe_runtime(work, PROBE)
        for mode in ("-O0", "-O2"):
            binary = work / ("settings" + mode)
            initial = sanitized(ir, runtime, binary, mode)
            assert initial.returncode == 2 and initial.stdout.startswith("usage:"), initial
            commands(binary, path)
        if args.negative_control:
            anchor = "addr(key_bytes), addr(value_bytes), 1)"
            assert original.count(anchor) == 1
            source.write_text(original.replace(anchor, "addr(key_bytes), addr(value_bytes), 0)"))
            emit_ir(args.compiler.resolve(), source, ir)
            binary = work / "no-commit"
            assert sanitized(ir, runtime, binary, "-O0").returncode == 2
            try:
                run(binary, ["set", "ui.dark", "1"], 0, None,
                    key="ui.dark", value="1", calls=1)
            except AssertionError as error:
                assert "c == 1" in str(error), error
            else:
                raise AssertionError("settings test accepted a write without immediate commit")
            print("PASS settings negative control: native write with commit=0 rejected by ABI assertion")
    print("PASS settings CLI: all commands, failures, exact file bytes; O0/O2 ASan/UBSan")


if __name__ == "__main__":
    main()
