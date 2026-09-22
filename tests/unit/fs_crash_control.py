#!/usr/bin/env python3
"""Prove the real crash sweep detects reusing old file blocks before commit.

Only a private source copy changes. Counted replacement makes source drift a
hard apparatus failure, and the verdict requires the overwrite atomicity error
rather than accepting a compiler error, a sanitizer fault or an arbitrary exit.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "c/fs/logitfs/logitfs.c"
DEFERRED = """static void bfree(uint32_t b)
{
    if (!b || b >= sb.total_blocks || !freed) return;
    if (!(freed[b >> 3] & (1 << (b & 7)))) nfreed++;
    freed[b >> 3] |= (uint8_t)(1u << (b & 7));
}"""
IMMEDIATE = """static void bfree(uint32_t b)
{
    /* Historical bug, in this generated control only. */
    if (b && b < sb.total_blocks) bit_clear(b);
}"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--flags", required=True)
    parser.add_argument("--sources", nargs="+", required=True)
    args = parser.parse_args()
    out = args.build.resolve() / "fs-crash-control"
    out.mkdir(parents=True, exist_ok=True)
    original = SOURCE.read_text()
    if original.count(DEFERRED) != 1:
        raise RuntimeError("bfree changed: review the control mutation before running it")
    if sum((ROOT / name).resolve() == SOURCE for name in args.sources) != 1:
        raise RuntimeError("FS_CORE must contain exactly one production LogitFS source")
    mutant = out / "logitfs.c"
    mutant.write_text(original.replace(DEFERRED, IMMEDIATE, 1))
    sources = [str(mutant) if (ROOT / name).resolve() == SOURCE else name
               for name in args.sources]
    binary = out / "crash"
    command = (shlex.split(args.cc) + shlex.split(args.flags) +
               ["-o", str(binary), "tests/unit/fs_crash_test.c"] + sources)
    record = {"compile": command,
              "source_sha256": hashlib.sha256(original.encode()).hexdigest(),
              "mutation": "bfree immediately clears allocation bits", "passed": False}
    try:
        with (out / "build.log").open("w") as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"control did not compile; see {out / 'build.log'}")
        with (out / "run.log").open("w") as log:
            result = subprocess.run([str(binary)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        output = (out / "run.log").read_text()
        failures = re.findall(r"^  FAIL: overwrite cut@\d+: /bys2 must be entirely "
                              r"the old content or entirely the new$", output, re.M)
        record.update(exit=result.returncode, overwrite_failures=len(failures))
        sanitizer = "ERROR: AddressSanitizer" in output or "runtime error:" in output
        if result.returncode != 1 or not failures or sanitizer:
            raise RuntimeError(f"control missed the overwrite atomicity verdict; see {out / 'run.log'}")
        record["passed"] = True
        print(failures[0])
        print(f"PASS: crash control caught {len(failures)} overwrite atomicity failures")
    finally:
        (out / "result.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
