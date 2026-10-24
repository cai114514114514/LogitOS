"""Exact fault sets come from the same pixel CSV as host and guest."""
import csv
import pathlib
import subprocess
import sys

binary, mode, logfile = sys.argv[1:]
rows = list(csv.reader(pathlib.Path(
    "tests/fixtures/engine-expansion/css-gradients-pixels.csv").open()))
expected = {"FAIL: " + row[4] for row in rows
            if row[0] == "pixel" and len(row) > 5
            and mode in row[5].split(";")}
assert expected, f"unknown or unobservable gradient control {mode}"
run = subprocess.run([binary], text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT)
pathlib.Path(logfile).write_text(run.stdout)
print(run.stdout, end="")
actual = [line for line in run.stdout.splitlines() if line.startswith("FAIL: ")]
summary = f"css-gradient-pixels: {len(rows)} checks, {len(expected)} failures"
assert run.returncode == 1, (mode, run.returncode, run.stdout)
assert len(actual) == len(expected) and set(actual) == expected, (
    mode, "missing", expected - set(actual), "extra", set(actual) - expected)
assert run.stdout.rstrip().endswith(summary), (mode, run.stdout)
print(f"css-gradient {mode}: observed exactly {len(expected)} expected pixel failures")
