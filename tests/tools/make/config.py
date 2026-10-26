#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise command tracking with real C objects, including its absent-stamp control."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]
TRACKING = ROOT / "tools/build/config.mk"
HELPER = ROOT / "tools/build/config.py"
FAILURE = "kernel flag change kept stale object"


def run(command: list[str], directory: Path, env: dict | None = None) -> str:
    result = subprocess.run(command, cwd=directory, env=env,
                            text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode:
        raise AssertionError(f"command failed: {command!r}\n{result.stdout}")
    return result.stdout


def make_programs() -> list[str]:
    programs = []
    for candidate in ("/usr/bin/make", shutil.which("gmake"), shutil.which("make")):
        if candidate and Path(candidate).exists() and candidate not in programs:
            version = run([candidate, "--version"], ROOT).splitlines()[0]
            if "GNU Make" in version:
                programs.append(candidate)
    if not programs:
        raise AssertionError("GNU Make is required to test the build graph")
    return programs


class Fixture:
    def __init__(self, directory: Path, make: str, broken: bool):
        self.directory = directory
        self.make = make
        self.env = dict(os.environ)
        for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
            self.env.pop(key, None)
        # This value passes through eval, Make expansion and the recipe shell.
        # None of its command substitutions may execute during configuration.
        self.metadata = "quote' double\" hash# dollar$ `touch injected` $(touch injected)"
        self.env["CONFIG_METADATA"] = self.metadata.replace("$", "$$")
        compiler = shutil.which("clang") or shutil.which("cc")
        if compiler is None:
            raise AssertionError("C compiler required; no compile was measured")
        self.compilers = [directory / "compiler one's", directory / "compiler two"]
        self.wrapper = ('#!/bin/sh\n[ "$CONFIG_FAIL" != 1 ] || exit 47\nexec '
                        + shlex.quote(compiler) + ' "$@"\n')
        for path in self.compilers:
            path.write_text(self.wrapper)
            path.chmod(0o755)
        (directory / "input.c").write_text("int value(void) { return VALUE; }\n")
        (directory / "main.c").write_text("int value(void); int main(void) { return value(); }\n")
        self.compiler = compiler
        (directory / "helper.py").write_text(
            "from pathlib import Path\nimport runpy\n"
            "with Path('checks').open('a') as f: f.write('check\\n')\n"
            f"runpy.run_path({str(HELPER)!r}, run_name='__main__')\n")
        dependencies = "" if broken else """
$(BUILD)/kernel.o: $(call config_stamp,kernel)
$(BUILD)/peer.o: $(call config_stamp,kernel)
$(BUILD)/user.o: $(call config_stamp,user)
$(BUILD)/variant.o: $(call config_stamp,variant)
"""
        finalize = "" if broken else "$(eval $(call build_config_finish))"
        # Variable NAMES cross eval, never raw values. In particular, the single
        # quote in CC and the #/$ in metadata must survive byte for byte.
        (directory / "fixture.mk").write_text(f"""
.DEFAULT_GOAL := all
BUILD := build
CC := {shlex.quote(str(self.compilers[0]))}
KFLAGS := -DVALUE=1
UFLAGS := -DVALUE=3
VFLAGS := -UVALUE -DVALUE=4
META := $(CONFIG_METADATA)
include {TRACKING}
BUILD_CONFIG_HELPER := helper.py
$(eval $(call build_config,kernel,CC,KFLAGS META,$(BUILD)/kernel.o $(BUILD)/peer.o))
$(eval $(call build_config,user,CC,UFLAGS,$(BUILD)/user.o))
$(eval $(call build_config,variant,CC,UFLAGS VFLAGS,$(BUILD)/variant.o))
{finalize}
.PHONY: all
all: $(BUILD)/kernel.o $(BUILD)/peer.o $(BUILD)/user.o $(BUILD)/variant.o
{dependencies}
$(BUILD)/kernel.o: KFLAGS += -DONLY_KERNEL=1
$(BUILD)/kernel.o: input.c
\t@mkdir -p $(BUILD)
\t@$(CC) $(KFLAGS) -c $< -o $@
$(BUILD)/peer.o: input.c
\t@mkdir -p $(BUILD)
\t@$(CC) $(KFLAGS) -c $< -o $@
$(BUILD)/user.o: input.c
\t@mkdir -p $(BUILD)
\t@$(CC) $(UFLAGS) -c $< -o $@
$(BUILD)/variant.o: input.c
\t@mkdir -p $(BUILD)
\t@$(CC) $(UFLAGS) $(VFLAGS) -c $< -o $@
""")

    def build(self, *assignments: str) -> None:
        run([self.make, "--no-print-directory", "-rR", "-j4", "-f", "fixture.mk",
             *assignments], self.directory, self.env)

    def files(self) -> dict[str, tuple[int, bytes]]:
        return {str(path.relative_to(self.directory)): (path.stat().st_mtime_ns, path.read_bytes())
                for path in (self.directory / "build").rglob("*") if path.is_file()}

    def check_value(self, name: str, value: int) -> None:
        output = self.directory / "verify"
        run([self.compiler, "main.c", f"build/{name}.o", "-o", str(output)], self.directory)
        actual = subprocess.run([str(output)], check=False).returncode
        assert actual == value, f"{name} returns {actual}, expected {value}"


def case(make: str, broken: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="logit-config-") as temporary:
        fixture = Fixture(Path(temporary), make, broken)
        fixture.build()
        before = fixture.files()
        fixture.check_value("kernel", 1)
        fixture.check_value("user", 3)
        fixture.check_value("variant", 4)
        fixture.build()
        assert before == fixture.files(), "idle build modified objects or stamps"
        fixture.build("KFLAGS=-DVALUE=2")
        after = fixture.files()
        assert before["build/kernel.o"][1] != after["build/kernel.o"][1], FAILURE
        fixture.check_value("kernel", 2)
        for name in ("user", "variant"):
            for path in (f"build/{name}.o", f"build/.config/{name}.json"):
                assert before[path] == after[path], f"kernel flags rebuilt {path}"
        fixture.build()
        fixture.check_value("kernel", 1)
        assert fixture.files()["build/kernel.o"][1] == before["build/kernel.o"][1]
        snapshot = fixture.files()
        fixture.build("VFLAGS=-UVALUE -DVALUE=5")
        fixture.check_value("variant", 5)
        for name in ("kernel", "user"):
            assert fixture.files()[f"build/{name}.o"] == snapshot[f"build/{name}.o"]
        snapshot = fixture.files()
        fixture.build("CC=" + shlex.quote(str(fixture.compilers[1])), "VFLAGS=-UVALUE -DVALUE=5")
        for name in ("kernel", "user", "variant"):
            assert fixture.files()[f"build/{name}.o"][0] != snapshot[f"build/{name}.o"][0], \
                f"compiler command change did not rebuild {name}"
        snapshot = fixture.files()
        fixture.compilers[1].write_text(fixture.wrapper + "# replaced compiler wrapper\n")
        fixture.build("CC=" + shlex.quote(str(fixture.compilers[1])), "VFLAGS=-UVALUE -DVALUE=5")
        for name in ("kernel", "user", "variant"):
            assert fixture.files()[f"build/{name}.o"][0] != snapshot[f"build/{name}.o"][0], \
                f"compiler replacement did not rebuild {name}"
        data = json.loads((fixture.directory / "build/.config/kernel.json").read_text())
        assert data["value"] == "-DVALUE=1 " + fixture.metadata, "configuration quoting changed bytes"
        assert not (fixture.directory / "injected").exists(), "configuration executed shell input"
        assert (fixture.directory / "checks").read_text().splitlines() == ["check"] * 13, \
            "expected one read-only check per make and one writer per changed build"


def partial_case(make: str) -> None:
    with tempfile.TemporaryDirectory(prefix="logit-config-partial-") as temporary:
        fixture = Fixture(Path(temporary), make, broken=False)
        fixture.build()
        fixture.build("build/kernel.o", "KFLAGS=-DVALUE=2")
        fixture.check_value("kernel", 2)
        fixture.check_value("peer", 1)
        snapshot = fixture.files()
        fixture.build("build/kernel.o", "KFLAGS=-DVALUE=2")
        assert snapshot == fixture.files(), "unrequested stale sibling rebuilt the current object"
        fixture.build("KFLAGS=-DVALUE=2")
        fixture.check_value("peer", 2)
        assert fixture.files()["build/kernel.o"] == snapshot["build/kernel.o"]
        snapshot = fixture.files()
        output = run([make, "-rR", "-n", "-f", "fixture.mk", "KFLAGS=-DVALUE=3"],
                     fixture.directory, fixture.env)
        assert "-o build/kernel.o" in output and "-o build/peer.o" in output
        assert snapshot == fixture.files(), "dry run consumed a configuration change"
        fixture.build("KFLAGS=-DVALUE=3")
        fixture.check_value("kernel", 3)
        fixture.check_value("peer", 3)
        fixture.env["CONFIG_FAIL"] = "1"
        result = subprocess.run([make, "-rR", "-f", "fixture.mk", "build/kernel.o", "KFLAGS=-DVALUE=4"],
                                cwd=fixture.directory, env=fixture.env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert result.returncode != 0 and "Error 47" in result.stdout, result.stdout
        fixture.env.pop("CONFIG_FAIL")
        fixture.build("build/kernel.o", "KFLAGS=-DVALUE=4")
        fixture.check_value("kernel", 4)
        fixture.check_value("peer", 3)


def cross_make_case(programs: list[str]) -> None:
    with tempfile.TemporaryDirectory(prefix="logit-config-cross-") as temporary:
        directory = Path(temporary)
        fixture = Fixture(directory, programs[0], broken=False)
        fixture.build()
        snapshot = fixture.files()
        # Separate per-version fixtures hid a real cross-version failure:
        # Make 3.81 adds a space for += EMPTY, whereas Make 4 omits it. Reusing
        # the same outputs is what proves switching entrypoints is a no-op.
        for make in programs[1:] + programs:
            fixture.make = make
            fixture.build()
            assert fixture.files() == snapshot, f"switching to {make} rebuilt unchanged outputs"
        fixture.build("KFLAGS=-DVALUE=2")
        snapshot = fixture.files()
        fixture.make = programs[0]
        fixture.build("KFLAGS=-DVALUE=2")
        assert fixture.files() == snapshot, "switching Make rebuilt the updated configuration"

        # A config group may carry two independent knobs, not just compiler
        # argv. Trimming its value would merge (enabled, empty) and (empty,
        # enabled), silently reusing the wrong diagnostic/control object.
        arguments = [sys.executable, str(HELPER), "--directory=" + str(directory / "slots"),
                     "--group=slots", "--tool=clang", "--outputs="]
        run(arguments + ["--value=1 "], directory)
        changed = run(arguments + ["--value= 1", "--check"], directory)
        assert "group=slots" in changed, "independent empty configuration slots collided"

        # Use the real product flags too. A miniature fixture without the
        # empty UEXTRA append cannot catch its version-dependent whitespace.
        # The private target writes snapshots only; it never requests a product
        # object, archive or disk, even when the caller is building one nearby.
        overlay = directory / "snapshot.mk"
        # The groups go through the same per-group append + @file path the real
        # build-config-update uses: $(BUILD_CONFIG_ARGS) on ONE recipe line is
        # ~175 KB, and Linux refuses a single argument over 128 KiB (E2BIG).
        overlay.write_text(".PHONY: config-test-snapshot\nconfig-test-snapshot:\n"
                           "\t@mkdir -p $(call build_quote,$(BUILD)/.config) && "
                           ": > $(call build_quote,$(BUILD_CONFIG_ARGFILE))\n"
                           "\t$(foreach group,$(BUILD_CONFIG_GROUPS),"
                           "$(call build_config_append_group,$(group)))\n"
                           "\t@python3 $(call build_quote,$(BUILD_CONFIG_HELPER)) "
                           "--directory=$(call build_quote,$(CONFIG_TEST_SNAPSHOT)) "
                           "$(call build_quote,@$(BUILD_CONFIG_ARGFILE)); rc=$$?; "
                           "rm -f $(call build_quote,$(BUILD_CONFIG_ARGFILE)); exit $$rc\n")
        expected = None
        for index, make in enumerate(programs):
            destination = directory / f"snapshot-{index}"
            run([make, "--no-print-directory", "-s", "-f", "Makefile", "-f", str(overlay),
                 "BUILD=" + str(directory / "products"), "JOBS=1",
                 "CONFIG_TEST_SNAPSHOT=" + str(destination), "config-test-snapshot"], ROOT, fixture.env)
            actual = {path.name: path.read_bytes() for path in destination.glob("*.json")}
            assert actual, "product configuration snapshot was empty"
            if expected is not None:
                different = sorted(name for name in actual.keys() | expected.keys()
                                   if actual.get(name) != expected.get(name))
                assert not different, f"Make entrypoints disagree on product flags: {different}"
            expected = actual
    print("config: shared outputs survive Make switching; independent empty slots remain distinct")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative-control", action="store_true")
    parser.add_argument("--broken-case")
    args = parser.parse_args()
    if args.broken_case:
        case(args.broken_case, broken=True)
        raise AssertionError("absent-stamp control unexpectedly passed")
    programs = make_programs()
    for make in programs:
        version = run([make, "--version"], ROOT).splitlines()[0]
        if args.negative_control:
            result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--broken-case", make],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            assert result.returncode != 0 and FAILURE in result.stdout, \
                f"negative control failed for an unrelated reason:\n{result.stdout}"
            print(f"config control ({version}): FAIL: {FAILURE}")
        else:
            case(make, broken=False)
            partial_case(make)
            print(f"config ({version}): flags, compiler, quoting, idle, partial builds and failure recovery pass")
    if not args.negative_control:
        cross_make_case(programs)


if __name__ == "__main__":
    main()
