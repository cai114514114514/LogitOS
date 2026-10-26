#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Update separate build-configuration stamps without touching unchanged files.

Make's dependency graph knows file dates, not command lines. The caller passes
one group for each distinct product compiler/flags domain. A single invocation
checks all groups so an idle build does not start Python once per object or
compiler domain. Commands are data here: never evaluate them in a shell.
"""

from __future__ import annotations

import argparse
from functools import lru_cache
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import tempfile


@lru_cache(maxsize=None)
def tool_identity(command: str) -> list[dict]:
    """Notice PATH changes and replaced compiler files without spawning tools."""
    result = []
    # CC may include a wrapper ("env ... clang" or "ccache clang"). Include all
    # executable words in that command, not only the first wrapper. Flags live
    # in a separate argument and never become filesystem or shell operations.
    for word in shlex.split(command):
        if word.startswith("-") or "=" in word:
            continue
        executable = shutil.which(word)
        if executable is None:
            continue
        path = Path(executable).resolve()
        stat = path.stat()
        result.append({
            "word": word,
            "path": str(path),
            "size": stat.st_size,
            "mtime_ns": stat.st_mtime_ns,
            "ctime_ns": stat.st_ctime_ns,
        })
    # Missing tools remain missing: config inspection must not turn "make
    # clean" or an unrelated target into a toolchain installation requirement.
    # A requested compile still reports its ordinary command-not-found error.
    return result


def stamp_content(command: str, value: str) -> bytes:
    return json.dumps({
        "command": command,
        "tools": tool_identity(command),
        "value": value,
    }, sort_keys=True, ensure_ascii=True, indent=2).encode() + b"\n"


def read_stamp(path: Path) -> bytes | None:
    try:
        return path.read_bytes()
    except FileNotFoundError:
        return None


def stale_outputs(path: Path, outputs: str) -> list[str]:
    """Make 3.81 on macOS loses nanoseconds; check that boundary ourselves."""
    stamp_time = path.stat().st_mtime_ns
    stale = []
    for output in outputs.split():
        try:
            if Path(output).stat().st_mtime_ns < stamp_time:
                stale.append(output)
        except FileNotFoundError:
            # Make already rebuilds absent outputs. Forcing their whole group
            # would turn deletion of one object into a full library rebuild.
            continue
    return stale


def update_stamp(path: Path, content: bytes) -> None:
    if read_stamp(path) == content:
        return

    path.parent.mkdir(parents=True, exist_ok=True)
    # Readers must see the old complete document or the new complete document,
    # including when a build is interrupted while writing its configuration.
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(content)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main() -> None:
    # "@FILE" reads one argument per line. The Makefile cannot pass every group on the
    # command line (Linux caps one argument at 128 KiB), so it writes them to a file.
    parser = argparse.ArgumentParser(description=__doc__, fromfile_prefix_chars="@")
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--group", action="append", default=[])
    parser.add_argument("--tool", action="append", default=[])
    parser.add_argument("--value", action="append", default=[])
    parser.add_argument("--outputs", action="append", default=[])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not (len(args.group) == len(args.tool) == len(args.value) == len(args.outputs)):
        parser.error("each group needs exactly one --tool, --value and --outputs")
    if len(set(args.group)) != len(args.group):
        parser.error("configuration group names must be unique")
    for group, command, value, outputs in zip(args.group, args.tool, args.value, args.outputs):
        if re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]*", group) is None:
            parser.error(f"invalid configuration group: {group!r}")
        try:
            path = args.directory / f"{group}.json"
            content = stamp_content(command, value)
            if args.check:
                if read_stamp(path) != content:
                    print(f"group={group}")
                    stale = outputs.split()
                else:
                    stale = stale_outputs(path, outputs)
                for output in stale:
                    print(f"output={output}")
            else:
                update_stamp(path, content)
        except (OSError, ValueError) as error:
            parser.error(f"{group}: {error}")


if __name__ == "__main__":
    main()
