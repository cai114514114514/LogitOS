#!/usr/bin/env python3
"""Run compiled loader gates only after their actual MMU capability probe.

Compilation happens in make before this runner, even on unsupported hosts:
missing allocator or crypto dependencies must remain errors, never SKIPs.
"""
import argparse
import errno
import os
from pathlib import Path
import signal
import subprocess
import sys


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], **kwargs)


def available(probe, mode):
    try:
        result = run([probe], capture_output=True, text=True,
                     env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    except OSError as error:
        # Cross-compilation succeeds even when Darwin has no Rosetta runtime.
        # Only an unsupported executable format is a capability result; a
        # missing probe, permissions error or arbitrary crash is a broken gate.
        unsupported = {errno.ENOEXEC}
        if sys.platform == "darwin":
            unsupported.add(86)  # EBADARCH
        if error.errno not in unsupported:
            raise
        reason = f"cannot execute the compiled host fixture ({error.strerror})"
    else:
        if result.returncode == 0:
            print(result.stdout.strip(), flush=True)
            return True
        if result.returncode != 77:
            raise RuntimeError(f"host probe failed ({result.returncode}): "
                               f"{result.stdout}{result.stderr}")
        reason = result.stdout.strip()
    print(f"SKIP: test-exec{mode}: {reason}; run make test-exec test-exec-fuzz "
          "test-exec-asan on Linux with 4 KiB pages or Darwin with an executable "
          "x86_64 runtime. No loader assertions or negative controls ran.", flush=True)
    return False


def recorded(command, path):
    result = run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    path.write_text(result.stdout)
    return result.returncode, result.stdout


def controls(build, seeds):
    code, output = recorded([build / "exec_test_ovf", build / "echo.aex"],
                            build / "exec_negctl_ovf.log")
    if (code not in (-signal.SIGSEGV, -signal.SIGBUS)
            or "ok: p_vaddr + p_memsz overflowing" not in output
            or "ok: p_memsz = -0x1000" in output
            or "ok: the reference image loads" not in output):
        raise RuntimeError("overflow control did not fault on the named overflow case")
    print("ok: overflow unit control faults on the named overflow case", flush=True)
    code, _ = recorded([build / "exec_fuzz_ovf", "40000", "1", *seeds],
                       build / "exec_negctl_fuzz.log")
    if code not in (-signal.SIGSEGV, -signal.SIGBUS):
        raise RuntimeError(f"overflow fuzzer returned {code}; no identified overflow failure")
    # exec_fuzz only prints progress after the full run. Even the expected
    # signal cannot establish that seeds loaded or the named bug was reached.
    # Keep this extra observation, but the unit case above supplies the actual
    # overflow control verdict. A missing library, abort or exit is never green.
    print(f"OBSERVATION: overflow fuzzer received {signal.Signals(-code).name}; "
          "the fixture has no case progress marker, so this is NOT a passing "
          "fuzz control.", flush=True)

    code, output = recorded([build / "exec_test_nowx", build / "sh.aex"],
                            build / "exec_negctl_nowx.log")
    if (code != 1 or "writable=1, p_flags says 0" not in output
            or "ok: the reference image loads" not in output):
        raise RuntimeError("W^X control did not fail the segment permission assertions")
    print("ok: W^X control fails the segment permissions", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--mode", choices=("test", "fuzz", "asan", "control"), required=True)
    parser.add_argument("images", nargs="+")
    args = parser.parse_args()
    build = args.build.resolve()
    suffix = "_asan" if args.mode == "asan" else ""
    label = {"test": "", "control": "-negctl", "fuzz": "-fuzz", "asan": "-asan"}
    if not available(build / ("exec_probe" + suffix), label[args.mode]):
        return 0
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
    if args.mode == "control":
        controls(build, args.images)
        return 0
    if args.mode in ("test", "asan"):
        result = run([build / ("exec_test" + suffix), *args.images], env=env)
        if result.returncode:
            return 1
    if args.mode == "test":
        return 0
    for seed in ([7] if args.mode == "asan" else [1, 2, 3]):
        count = 8000 if args.mode == "asan" else 40000
        result = run([build / ("exec_fuzz" + suffix), count, seed, *args.images], env=env)
        if result.returncode:
            return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: exec host gate: {error}", file=sys.stderr)
        sys.exit(1)
