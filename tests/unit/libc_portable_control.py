#!/usr/bin/env python3
"""Prove portable libc assertions reject broken production behavior.

Mutations are exact single-anchor edits to private source copies. No production
test flag or substituted reference implementation is involved. Build/link errors
and crashes never satisfy a control: the named assertion must fail with exit 1.
"""

import argparse
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]

CONTROLS = {
    "search": [
        ("find", "if (c == 0) return (void *)n;",
         "if (c == 0) return NULL;", "tree lookup: FAIL"),
        ("destroy", "if (freefct) freefct((void *)n->key);",
         "(void)freefct;", "FAIL search contract: tdestroy calls callback for each key"),
    ],
    "uio": [
        ("position", "lseek(fd, saved, SEEK_SET);", "(void)saved;",
         "preadv preserves cursor: FAIL"),
        ("progress", "if (n < 0) return moved > 0 ? moved : -1;             /* rule 2 */",
         "if (n < 0) return -1; /* control: hide earlier progress */",
         "FAIL uio contract: read error after progress returns count"),
    ],
}


def run(command, log):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
    log.write_text(result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--family", choices=CONTROLS, required=True)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    out = args.build.resolve() / "portable-control" / args.family
    out.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(args.cc)
    resource = subprocess.check_output(compiler + ["-print-resource-dir"], text=True).strip()
    includes = ["-nostdinc", "-isystem", resource + "/include",
                "-Ic/apps/libc/include", "-Ic/apps/libc/include/uonly", "-Ic/apps/libc/src"]
    extras = []
    if args.family == "uio":
        helper = out / "host.o"
        command = compiler + ["-std=c11", "-O1", "-c", "tests/unit/libc_uio_host.c", "-o", str(helper)]
        result = run(command, out / "helper-build.log")
        if result.returncode:
            raise RuntimeError("host setup compilation failed; see " + str(out / "helper-build.log"))
        extras.append(str(helper))
    production = (ROOT / "c/apps/libc/src" / (args.family + ".c")).read_text()
    for name, anchor, replacement, assertion in CONTROLS[args.family]:
        if production.count(anchor) != 1:
            raise RuntimeError(f"{args.family}/{name}: mutation anchor is not unique")
        source = out / (name + ".c")
        source.write_text(production.replace(anchor, replacement, 1))
        binary = out / name
        command = compiler + ["-std=c11", "-O1", "-Wall", "-Wextra",
            "-DLIBC_COMMON_TEST", "-DLIBC_OURS_HOST_TEST", *includes,
            "tests/unit/libc_" + args.family + "_test.c", str(source),
            "tests/unit/libc_host_errno_shim.c", *extras, "-o", str(binary)]
        (out / (name + "-command.json")).write_text(json.dumps(command, indent=2) + "\n")
        compiled = run(command, out / (name + "-build.log"))
        if compiled.returncode:
            raise RuntimeError(f"{args.family}/{name}: control failed to compile")
        result = run([str(binary)], out / (name + ".log"))
        if result.returncode != 1 or assertion not in result.stdout:
            raise RuntimeError(f"{args.family}/{name}: expected exit 1 and {assertion!r}; see {out}")
        print(f"CONTROL {args.family}/{name}: observed {assertion}", flush=True)


if __name__ == "__main__":
    main()
