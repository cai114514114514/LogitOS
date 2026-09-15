#!/usr/bin/env python3
"""Match the exact finite-task failures restored by each scheduler control."""
import collections
import pathlib
import re
import sys


def expected(mode):
    failures = collections.Counter()
    if mode == "unbounded":
        failures.update({
            "ready messages yield after first complete task": 1,
            "startup buffered messages yield after first complete task": 1,
            "page scheduler shares worker turn budget": 1,
            "page parent-result handoff returns before another worker native call": 1,
            "parent input is handled before second worker addition": 3,
            "second addition observes intervening parent input": 3,
            "fetch-only checkpoint yields after first complete addition": 1,
            "fetch-only remaining microtask remains scheduler-pending": 2,
            "fetch-only remaining microtask retains an immediate due time": 2,
            "remaining worker microtask resumes after parent input without loss": 2,
            "fetch checkpoint yields before its remaining job and later task": 1,
            "terminate scenario reaches a completed-task budget boundary": 1,
            "page-close scenario reaches a completed-task budget boundary": 1,
            "two fetch owners share one turn budget": 1,
            "queued parent message dispatch precedes next owner fetch reaction": 1,
        })
    elif mode == "hidden-jobs":
        failures.update({
            "fetch-only remaining microtask remains scheduler-pending": 2,
            "fetch-only remaining microtask retains an immediate due time": 2,
            "remaining worker microtask resumes after parent input without loss": 1,
            "fetch-only worker eventually delivers unchanged result 42": 1,
        })
    elif mode == "no-parent-sweep":
        failures["page parent-result handoff returns before another worker native call"] = 1
        failures["queued parent message dispatch precedes next owner fetch reaction"] = 1
    elif mode != "current":
        raise ValueError("unknown mode")
    return failures


def main():
    mode, filename = sys.argv[1:]
    data = pathlib.Path(filename).read_text()
    actual = collections.Counter(re.findall(r"^FAIL: (.+)$", data, re.M))
    wanted = expected(mode)
    summaries = re.findall(r"^worker-fairness: (\d+) checks, (\d+) failures$", data, re.M)
    okay = actual == wanted and summaries == [("60", str(sum(wanted.values())))]
    okay = okay and "[js exception]" not in data
    if not okay:
        print("worker-fairness control mismatch:", mode)
        print("missing failures:", dict(wanted - actual))
        print("unexpected failures:", dict(actual - wanted))
        print("summaries:", summaries)
        return 1
    print(f"worker-fairness {mode}: exact 60-check matrix; {sum(wanted.values())} expected failures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
