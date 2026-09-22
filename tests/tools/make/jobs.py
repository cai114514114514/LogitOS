#!/usr/bin/env python3
"""Exercise the real jobs policy with independent make graphs, never the OS.

Workers count overlapping recipes under flock. Their rendezvous tests scheduling
without treating host wall time as a performance measurement. A recursive graph
shares the same counter so a submake that creates its own pool exceeds the bound.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[3]
POLICY = ROOT / "tools/build/jobs.mk"
WORKERS = 16


def state_change(path, delta=0):
    with open(path, "a+", encoding="utf-8") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        stream.seek(0)
        state = json.load(stream)
        state["active"] += delta
        state["peak"] = max(state["peak"], state["active"])
        stream.seek(0)
        stream.truncate()
        json.dump(state, stream)
        return state


def worker(path, rendezvous):
    state_change(path, 1)
    try:
        deadline = time.monotonic() + 15
        while state_change(path)["peak"] < rendezvous:
            if time.monotonic() > deadline:
                raise AssertionError("worker rendezvous did not reach the requested job count")
            time.sleep(0.01)
        # Keep the counter observable until every member of the first batch has
        # seen it. This delay is apparatus, never a build-speed measurement.
        time.sleep(0.01)
    finally:
        state_change(path, -1)


def make_programs():
    programs = []
    seen = set()
    for name in ("gmake", "make", "/usr/bin/make"):
        found = shutil.which(name)
        if not found:
            continue
        resolved = str(Path(found).resolve())
        if resolved in seen:
            continue
        seen.add(resolved)
        result = subprocess.run([found, "--version"], capture_output=True, text=True, check=True)
        match = re.search(r"GNU Make (\d+)\.(\d+)", result.stdout, re.IGNORECASE)
        if match:
            programs.append((found, tuple(map(int, match.groups()))))
    return programs


def environment(extra):
    result = os.environ.copy()
    for name in ("MAKEFLAGS", "MFLAGS", "GNUMAKEFLAGS", "MAKELEVEL", "MAKEOVERRIDES", "JOBS"):
        result.pop(name, None)
    result.update(extra or {})
    return result


def fixture(path, policy, state, rendezvous):
    command = " ".join(map(shlex.quote, (sys.executable, str(Path(__file__).resolve()),
                                      "--worker", str(state), str(rendezvous))))
    path.write_text(
        ".DEFAULT_GOAL := all\n"
        "ISO := product.iso\nDISK := disk.img\n"
        f"include {policy}\n"
        "workers := " + " ".join(f"work-{n}" for n in range(WORKERS)) + "\n"
        ".PHONY: all run product.iso disk.img test-probe recursive recurse parent-a parent-b $(workers)\n"
        "all run product.iso disk.img test-probe: $(workers)\n"
        "\t@printf 'FLAGS [%s]\\n' '$(MAKEFLAGS)'\n"
        "$(workers) parent-a parent-b:\n"
        f"\t@{command}\n"
        "recursive: parent-a parent-b recurse\n"
        "recurse:\n"
        "\t@printf 'PARENT [%s]\\n' '$(MAKEFLAGS)'\n"
        "\t+@$(MAKE) --no-print-directory -f $(firstword $(MAKEFILE_LIST)) all\n",
        encoding="utf-8",
    )


def run_case(make, policy, label, arguments, expected, env=None, rendezvous=None):
    with tempfile.TemporaryDirectory(prefix="logit-jobs-") as directory:
        work = Path(directory)
        state = work / "state.json"
        state.write_text('{"active":0,"peak":0}', encoding="utf-8")
        graph = work / "Makefile"
        fixture(graph, policy, state, expected if rendezvous is None else rendezvous)
        result = subprocess.run([make, "--no-print-directory", "-f", str(graph), *arguments],
                                cwd=work, env=environment(env), capture_output=True,
                                text=True, timeout=40)
        if result.returncode:
            raise AssertionError(f"{label}: make exited {result.returncode}\n{result.stdout}{result.stderr}")
        if "jobserver" in result.stderr.lower():
            raise AssertionError(f"{label}: jobserver warning: {result.stderr}")
        observed = json.loads(state.read_text(encoding="utf-8"))
        if observed != {"active": 0, "peak": expected}:
            raise AssertionError(f"{label}: expected peak {expected}, got {observed}")
        if "recursive" in arguments:
            pools = re.findall(r"--jobserver-(?:auth|fds)=([^\s\]]+)", result.stdout)
            if expected > 1 and (len(pools) < 2 or len(set(pools)) != 1):
                raise AssertionError(f"{label}: parent and child did not share a jobserver: {pools}")
            if expected == 1 and pools:
                raise AssertionError(f"{label}: a serial parent unexpectedly created a jobserver: {pools}")
        print(f"PASS {Path(make).name}: {label} (peak {expected})")


def positive(programs):
    defaults = min(12, os.cpu_count() or 1)
    modern_seen = False
    for make, version in programs:
        modern = version >= (4, 4)
        modern_seen |= modern
        automatic = defaults if modern else 1
        for goals in ([], ["all"], ["run"], ["product.iso"], ["disk.img"], ["all", "run"]):
            run_case(make, POLICY, "product " + (" ".join(goals) or "default"), goals, automatic)
        run_case(make, POLICY, "JOBS override", ["JOBS=3"], 3 if modern else 1)
        run_case(make, POLICY, "environment JOBS", [], 4 if modern else 1, {"JOBS": "4"})
        for arguments, jobs in ((["-j1", "JOBS=5"], 1), (["-j2"], 2), (["-j"], WORKERS),
                                (["--jobs=3"], 3), (["-sj2"], 2)):
            run_case(make, POLICY, "explicit " + " ".join(arguments), arguments, jobs)
        for flags, jobs in (("-j1", 1), ("-j 3", 3), ("--jobs=2", 2)):
            run_case(make, POLICY, "MAKEFLAGS=" + flags, ["JOBS=5"], jobs, {"MAKEFLAGS": flags})
        run_case(make, POLICY, "command-line MAKEFLAGS", ["MAKEFLAGS=-j1", "JOBS=5"], 1)
        run_case(make, POLICY, "non-job MAKEFLAGS retained", [], automatic, {"MAKEFLAGS": "-s"})
        run_case(make, POLICY, "test stays serial", ["test-probe", "JOBS=5"], 1)
        run_case(make, POLICY, "mixed goals stay serial", ["all", "test-probe", "JOBS=5"], 1)
        run_case(make, POLICY, "recursive serial parent", ["recursive", "JOBS=9"], 1)
        run_case(make, POLICY, "recursive shared pool", ["-j3", "recursive", "JOBS=9"], 3)
    if not modern_seen:
        print("SKIP automatic jobs: GNU Make >= 4.4 unavailable; install gmake and run make test-build-jobs")


def negative(programs):
    modern = next((make for make, version in programs if version >= (4, 4)), None)
    if modern is None:
        print("SKIP jobs negative control: GNU Make >= 4.4 unavailable; install gmake and run make test-build-jobs")
        return
    # Break only the automatic policy. The fixture must still build, then the
    # independent active-recipe counter must reject its forced serial schedule.
    with tempfile.TemporaryDirectory(prefix="logit-jobs-negative-") as directory:
        broken = Path(directory) / "jobs.mk"
        source = POLICY.read_text(encoding="utf-8")
        original = "MAKEFLAGS += -j$(BUILD_AUTO_JOBS)"
        if source.count(original) != 1:
            raise AssertionError("negative-control injection point is missing or ambiguous")
        broken.write_text(source.replace(original, "MAKEFLAGS += -j1"), encoding="utf-8")
        try:
            run_case(modern, broken, "broken default", ["JOBS=3"], 3, rendezvous=1)
        except AssertionError as error:
            if "expected peak 3" not in str(error):
                raise
            print(f"NEGATIVE CONTROL observed failure: {error}")
        else:
            raise AssertionError("negative control passed with the policy forced serial")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative", action="store_true")
    parser.add_argument("--worker", nargs=2, metavar=("STATE", "RENDEZVOUS"))
    args = parser.parse_args()
    if args.worker:
        worker(args.worker[0], int(args.worker[1]))
        return
    programs = make_programs()
    if not programs:
        raise AssertionError("GNU make is required; install gmake and run make test-build-jobs")
    (negative if args.negative else positive)(programs)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"jobs: FAIL: {error}") from error
