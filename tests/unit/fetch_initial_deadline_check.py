"""Exact assertions, including the active-idle controls, for one finite gate."""
import collections
import pathlib
import sys

mode, path = sys.argv[1:]
lines = pathlib.Path(path).read_text().splitlines()
failures = collections.Counter(line[6:] for line in lines if line.startswith("FAIL: "))
expected = collections.Counter([
    "delayed first pump receives the ordinary response",
    "time zero is a valid initial observation baseline",
    "first-pump gap does not consume the serviced idle budget",
]) if mode == "old" else collections.Counter()
assert failures == expected, (failures, expected)
assert f"fetch-initial-deadline: 15 checks, {sum(expected.values())} failures" in lines
assert not any("[js exception]" in line for line in lines)
print(f"fetch-initial-deadline {mode}: exact assertion matrix accepted")
