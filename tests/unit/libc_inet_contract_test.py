#!/usr/bin/env python3
"""Exercise the mini-libc parser and mutations with its own headers.

Darwin pton is not an oracle for the project's strict leading-zero policy.
This test does not open sockets or make network requests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "c/apps/libc/src/inet.c"
FIXTURE = ROOT / "tests/unit/libc_inet_contract_test.c"


def run(command, log, expected=0):
    environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    result = subprocess.run(command, cwd=ROOT, env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write_text(result.stdout)
    if result.returncode != expected:
        raise RuntimeError(f"exit {result.returncode}, expected {expected}: {shlex.join(command)}\n{result.stdout}")
    return result.stdout


def mutate(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError(f"mutation anchor must occur once: {old!r}")
    return source.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--controls", action="store_true")
    args = parser.parse_args()
    output = args.build.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(args.cc)
    resource = subprocess.check_output(compiler + ["-print-resource-dir"], text=True).strip()
    includes = ["-nostdinc", "-isystem", resource + "/include",
                "-Ic/apps/libc/include", "-Ic/apps/libc/include/uonly"]
    source = SOURCE.read_text()
    modes = [("optimized", source, ["-O2"], None),
             ("sanitize", source, ["-O1", "-g", "-fsanitize=address,undefined",
                                   "-fno-omit-frame-pointer"], None)]
    if args.controls:
        modes = [
            ("trailing-colon", mutate(source, "} else if (!*cursor) {", "} else if (0) {"),
             ["-O1"], "FAIL pton reject [::1:]"),
            ("empty-compression", mutate(source,
                 "if (count == 8) return 0;\n        int tail = count - compression;",
                 "if (count > 8) return 0;\n        int tail = count - compression;"),
             ["-O1"], "FAIL pton reject [::1:2:3:4:5:6:7:8]"),
            ("decimal-zero", mutate(source,
                 "if (p[0] == '0' && isdigit((unsigned char)p[1])) return 0;",
                 "/* negative control: admit ambiguous leading zeroes */"),
             ["-O1"], "FAIL pton reject [1.2.3.04]"),
            ("aton-trailer", mutate(source,
                 "if (*p != 0 && !isspace((unsigned char)*p)) return 0;",
                 "if (*p != 0) return 0;"),
             ["-O1"], "FAIL aton accept [1.2.3.4 ]"),
        ]
    records = []
    for name, content, flags, marker in modes:
        variant = output / (name + ".c")
        variant.write_text(content)
        binary = output / name
        command = compiler + ["-std=c11", "-Wall", "-Wextra"] + flags + includes
        command += [str(FIXTURE), str(variant), "tests/unit/libc_host_errno_shim.c", "-o", str(binary)]
        run(command, output / (name + ".build.log"))
        result = run([str(binary)], output / (name + ".log"), expected=1 if marker else 0)
        if marker and marker not in result.splitlines():
            raise RuntimeError(f"control {name} failed without its intended assertion:\n{result}")
        print(f"{name}: {result.splitlines()[-1]}")
        if marker:
            print(f"observed control: {marker}")
        records.append({"name": name, "command": command,
                        "source_sha256": hashlib.sha256(content.encode()).hexdigest(),
                        "fixture_sha256": hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
                        "expected_exit": 1 if marker else 0, "summary": result.splitlines()[-1]})
    label = "controls" if args.controls else "positive"
    (output / (label + "-results.json")).write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()
