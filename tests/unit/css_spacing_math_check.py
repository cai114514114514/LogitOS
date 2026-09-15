#!/usr/bin/env python3
"""Check the complete ordinary geometry matrix, including exact old failures."""
from collections import Counter
from pathlib import Path
import re
import subprocess
import sys

binary, output, mode = sys.argv[1:]
literal = subprocess.run([binary, "literal"], capture_output=True, text=True)
assert literal.returncode == 0 and literal.stdout.strip() == "css-spacing-math: 12 checks, 0 failures", literal
assert not literal.stderr, literal.stderr
run = subprocess.run([binary], capture_output=True, text=True)
Path(output).write_text(run.stdout + run.stderr)
failures = Counter(re.findall(r"^  FAIL: (.*?) -- got -?\d+, want -?\d+$", run.stdout, re.M))
expected = Counter()
if mode == "legacy":
    for prefix in ("root relative length", "length multiplication", "expanded variable multiplication", "mixed length units"):
        for suffix in ("flex gap reaches actual second child", "grid gap reaches actual second track",
                       "logical inline padding reaches content origin", "logical block padding reaches content origin"):
            expected[prefix + ": " + suffix] += 1
    for label in (
        "later calc replaces earlier ordinary literal", "invalid literal suffix retains earlier valid value",
        "normal gap resets earlier value", "negative calculated gap is range clamped",
        "important calc beats normal inline", "element font resolves em after matching",
        "column longhand replaces one shorthand component", "later shorthand replaces earlier column longhand",
        "literal logical padding clears old physical percentage", "invalid logical suffix keeps earlier complete declaration",
        "negative logical literal keeps earlier declaration"):
        expected[label] += 1
if mode in ("legacy", "own-width"):
    expected["logical padding percentage is relative to containing width"] = 2
    for label in (
        "negative percentage calc resolves before range clamp", "mixed padding uses containing width and px addend",
        "vertical percentage padding also uses containing width", "resize recomputes logical inline percentage padding",
        "resize recomputes logical block percentage padding", "repeated layout does not accumulate inline padding",
        "repeated layout does not accumulate block padding"):
        expected[label] += 1
assert mode in ("current", "legacy", "own-width")
assert not run.stderr, run.stderr
assert run.returncode == (1 if expected else 0), (mode, run.returncode, run.stdout)
assert failures == expected, (mode, failures - expected, expected - failures, run.stdout)
assert run.stdout.count("  FAIL:") == sum(expected.values()), run.stdout
assert run.stdout.rstrip().endswith(f"css-spacing-math: 144 checks, {sum(expected.values())} failures"), run.stdout
print(f"css-spacing-math {mode}: literal 12/12; 144 checks, exact {sum(expected.values())} failures")
