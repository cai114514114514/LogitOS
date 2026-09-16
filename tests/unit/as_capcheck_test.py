#!/usr/bin/env python3
"""Exercise the migrated capability example against real files and guest grants.

Host runs relocate the two fixture paths into a private directory. Only the
host runtime's initial grant is replaced; file I/O and pointer checks stay real.
Guest runs use the unmodified source and SYS_CAP_SPAWN, never a simulated grant.
"""

import argparse
import json
from pathlib import Path
import shlex
import tempfile

from as_capability_test import kernel_grants
from as_managed_test import ROOT, emit_ir, sanitized
from as_runtime import copy_runtime

SOURCE = ROOT / "fsroot/as/examples/capcheck.as"


def expected_output(bits, prefix, etc, usr, raw, narrowed="/usr/as"):
    return (
        "capcheck: start\n"
        f"bits {bits}\n"
        f"path {prefix}\n"
        f"read-etc {etc}\n"
        f"read-usr {usr}\n"
        f"raw-peek {raw}\n"
        f"narrowed {narrowed}\n"
        "no-regain ok\n"
        "capcheck: done\n"
    )


def check_output(output, expected):
    # A whole transcript comparison prevents evidence from different children
    # being mixed together. In particular, root success cannot supply the
    # scoped child's read-usr result or its attenuation check.
    if output != expected:
        raise AssertionError(f"capcheck output differs: {output!r}; expected {expected!r}")


def guest_assets(work):
    grants = kernel_grants(work)
    kernel = 0
    language = 0
    for kernel_bits, language_bits, _ in grants.values():
        kernel |= kernel_bits
        language |= language_bits
    configuration = work / "capcheck-config.txt"
    configuration.write_text("native capability fixture\n")
    files = [
        (configuration, "/etc/logit.conf"),
        (ROOT / "fsroot/as/lib/sys.as", "/usr/as/lib/sys.as"),
    ]
    return files, {"kernel_all": kernel, "language_all": language}


def run_guest_capcheck(guest, executable, program, grants):
    bits = grants["language_all"]
    root = expected_output(bits, "None", "ok", "ok", "ok")
    scoped = expected_output(bits, "/usr/as", "denied", "ok", "ok")
    empty = expected_output(0, "None", "denied", "denied", "denied")
    cases = (
        ("root", [], root),
        ("scoped", ["--caps", str(grants["kernel_all"]), "/usr/as"], scoped),
        ("none", ["--caps", "0", ""], empty),
    )
    records = []
    for label, options, expected in cases:
        command = shlex.join(["/bin/native-capture", *options, executable])
        output = guest.capture(command, timeout=45)
        assert guest.last_capture_exit == 0, (label, output)
        check_output(output, expected)
        records.append({"grant": label, "command": command, "exit_code": 0, "output": output})

    # Observe the apparatus rejecting a real unscoped run as scoped evidence.
    # A launcher that forgets --caps must not pass on a final "done" marker.
    try:
        check_output(records[0]["output"], scoped)
    except AssertionError:
        pass
    else:
        raise AssertionError("unscoped native execution satisfied the scope oracle")
    program.update(exit_code=0, capability_runs=records, unscoped_control_rejected=True)


def host_checks(compiler, negative):
    with tempfile.TemporaryDirectory(prefix="as-capcheck-") as temporary:
        # macOS /var is a symlink. Scoped host access deliberately refuses
        # symlink traversal, so use the actual private directory as the grant.
        work = Path(temporary).resolve()
        _, grants = guest_assets(work)
        bits = grants["language_all"]
        filesystem = work / "filesystem"
        etc = filesystem / "etc"
        usr = filesystem / "usr/as"
        etc.mkdir(parents=True)
        (usr / "lib").mkdir(parents=True)
        (etc / "logit.conf").write_text("present outside the scoped path\n")
        (usr / "lib/sys.as").write_text("present inside the scoped path\n")

        # Relocate only path literals, keeping the real example's operations.
        original = SOURCE.read_text()
        relocated = original.replace('"/etc/logit.conf"', json.dumps(str(etc / "logit.conf")))
        relocated = relocated.replace('"/usr/as/lib/sys.as"', json.dumps(str(usr / "lib/sys.as")))
        relocated = relocated.replace('"/usr/as"', json.dumps(str(usr)))
        source = work / "capcheck.as"
        source.write_text(relocated)
        ir = work / "capcheck.ll"
        emit_ir(compiler, source, ir)

        runtime = work / "runtime"
        runtime.mkdir()
        copy_runtime(runtime)
        capability = runtime / "capability.c"
        original_runtime = capability.read_text()
        begin = original_runtime.index("    at_caps_set(AS_CAP_FS_READ", original_runtime.index("void at_caps_init"))
        end = original_runtime.index("#else", begin)

        cases = (
            ("root", bits, None, "ok", "ok", "ok"),
            ("scoped", bits, str(usr), "denied", "ok", "ok"),
            ("none", 0, None, "denied", "denied", "denied"),
        )
        for label, held, prefix, read_etc, read_usr, raw in cases:
            literal = "NULL" if prefix is None else json.dumps(prefix)
            replacement = f"    at_caps_set({held}, {literal});\n"
            capability.write_text(original_runtime[:begin] + replacement + original_runtime[end:])
            expected = expected_output(held, "None" if prefix is None else prefix,
                                       read_etc, read_usr, raw, str(usr))
            for mode in ("-O0", "-O2"):
                result = sanitized(ir, runtime, work / label, mode)
                assert result.returncode == 0, result
                check_output(result.stdout, expected)

        capability.write_text(original_runtime)
        (etc / "logit.conf").unlink()
        missing = sanitized(ir, runtime, work / "missing", "-O0")
        assert missing.returncode == 1 and "IOError" in missing.stderr, missing
        assert "capcheck: done" not in missing.stdout, missing

        if negative:
            # This recreates the old broad catch. A missing input then prints
            # "denied" and reaches done, which must be rejected as evidence of
            # functioning permissions despite the zero process exit status.
            source.write_text(relocated.replace("except PermissionError:", "except Error:"))
            emit_ir(compiler, source, ir)
            result = sanitized(ir, runtime, work / "broad-catch", "-O0")
            assert result.returncode == 0 and result.stdout.endswith("capcheck: done\n"), result
            expected = expected_output(bits, "None", "ok", "ok", "ok", str(usr))
            try:
                check_output(result.stdout, expected)
            except AssertionError:
                pass
            else:
                raise AssertionError("missing fixture was accepted as a permission result")
            print("PASS capcheck control: broad catch hid missing input, complete oracle rejected it")
    print("PASS native capcheck: root/scoped/empty grants, real I/O, owned pointer, O0/O2")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    host_checks(args.compiler.resolve(), args.negative_control)
