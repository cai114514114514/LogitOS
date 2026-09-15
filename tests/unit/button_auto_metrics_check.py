#!/usr/bin/env python3
"""Check the complete normal geometry failure matrix, not just a nonzero exit."""
import collections
import pathlib
import re
import subprocess
import sys

exe, log, mode = sys.argv[1:]
result = subprocess.run([exe], capture_output=True, text=True, timeout=30)
output = result.stdout + result.stderr
pathlib.Path(log).write_text(output)
print(output, end="")
actual = collections.Counter(re.findall(r"FAIL: (.*?)(?: -- got [^\n]+)?$", output, re.M))
old = collections.Counter({
    "auto button includes authored horizontal edges": 3,
    "auto button includes authored line-height and vertical edges": 3,
    "auto button label fits vertical content box": 14,
    "auto button label fits horizontal content box": 1,
    "auto button label remains one line": 4,
    "logical padding participates in native auto width": 1,
    "logical padding participates in native auto height": 1,
    "asymmetric padding and border count exactly once": 1,
    "authored line-height uses the same oracle as text placement": 1,
    "line-height alone enlarges auto native height": 1,
})
expected = old if mode == "legacy" else collections.Counter()
expected_rc = 1 if expected else 0
summary = f"button-auto-metrics: 110 checks, {sum(expected.values())} failures"
if result.returncode != expected_rc or actual != expected or summary not in output:
    raise SystemExit(f"unexpected {mode} button geometry result: rc={result.returncode}, failures={actual}")
print(f"button-auto-metrics {mode}: complete expected matrix verified")
