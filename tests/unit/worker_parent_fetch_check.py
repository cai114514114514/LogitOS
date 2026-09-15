#!/usr/bin/env python3
"""Require exact ordinary task/transport ordering, with no extra failures."""
import pathlib
import re
import sys

mode, filename = sys.argv[1:]
if mode not in ("current", "control"):
    raise SystemExit("unknown mode")
data = pathlib.Path(filename).read_text()
failures = re.findall(r"^FAIL: (.+)$", data, re.M)
expected = [] if mode == "current" else [
    "parent callback fetch actually sends before next worker native call",
    "parent microtask fetch actually sends before next worker native call",
]
actual_sequence = re.findall(
    r"^parent-fetch sequence (callback|microtask): first-add=(\d+) "
    r"fetch-created=(\d+) first-send=(\d+) second-add=(\d+)$", data, re.M)
order = ("1", "2", "3", "4") if mode == "current" else ("1", "2", "4", "3")
sequences = [(case, *order) for case in ("callback", "microtask")]
summary = re.findall(r"^parent-fetch-order: (\d+) checks, (\d+) failures$", data, re.M)
okay = failures == expected and actual_sequence == sequences
okay = okay and summary == [("18", str(len(expected)))] and "[js exception]" not in data
if not okay:
    print("parent-fetch-order matrix mismatch:", mode)
    print("failures:", failures)
    print("sequences:", actual_sequence)
    print("summary:", summary)
    raise SystemExit(1)
print(f"parent-fetch-order {mode}: exact 18 checks and both transport sequences")
