#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove a fresh BUILD directory is an actual Make prerequisite target."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
MAKEFILE = ROOT / "Makefile"
MAKE = shutil.which("gmake") or shutil.which("make")
DIRECTORY_RULE = '$(BUILD):\n\t@mkdir -p "$@"\n'


def check_makefile_rule() -> str:
    source = MAKEFILE.read_text()
    if source.count(DIRECTORY_RULE) != 1:
        raise AssertionError("expected one BUILD directory rule in Makefile")
    return source


def run(makefile: Path, build: Path) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
        env.pop(name, None)
    return subprocess.run(
        [MAKE, "-f", str(makefile), f"BUILD={build}", "check-roots"],
        cwd=ROOT, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False,
    )


def control(source: str) -> None:
    with tempfile.TemporaryDirectory(prefix="logitos-build-dir-control-") as name:
        directory = Path(name)
        shadow = directory / "Makefile"
        shadow.write_text(source.replace(DIRECTORY_RULE, "", 1))
        build = directory / "missing-build"
        result = run(shadow, build)
        if (result.returncode == 0 or
                "No rule to make target" not in result.stdout or
                str(build) not in result.stdout or build.exists()):
            raise AssertionError(
                "removing the directory rule did not reproduce the old failure:\n"
                + result.stdout
            )
        print("NEGCTL RED: fresh BUILD prerequisite has no rule when creation is removed")


def positive() -> None:
    with tempfile.TemporaryDirectory(prefix="logitos-build-dir-") as name:
        build = Path(name) / "new-build"
        result = run(MAKEFILE, build)
        if result.returncode or not build.is_dir() or not (build / "roots_bundle.check").is_file():
            raise AssertionError(
                "fresh BUILD did not run check-roots through the real graph:\n"
                + result.stdout
            )
        print("build-dir: fresh isolated BUILD created; check-roots completed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--control", action="store_true")
    args = parser.parse_args()
    source = check_makefile_rule()
    if args.control:
        control(source)
    else:
        positive()
