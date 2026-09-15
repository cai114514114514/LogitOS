#!/usr/bin/env python3
"""Select packaged A3 examples and their explicit guest launch commands.

This reads the repository's mandatory first-line declaration, not arbitrary
editor input. The compiler remains responsible for validating the selected
version and building its source. A missing native artifact never falls back
to source execution through the retiring VM.
"""

import argparse
from pathlib import Path
import re
import shlex


def source_version(path):
    first = Path(path).read_text(encoding="utf-8").splitlines()
    match = re.fullmatch(r"# aether: (2|3(?:\.[0-9]+)?)", first[0] if first else "")
    if not match:
        raise ValueError(f"Shipped example needs an explicit A2/A3 declaration: {path}")
    return int(match[1].split(".")[0])


def guest_command(path, background=False):
    path = Path(path)
    if source_version(path) == 3:
        command = shlex.quote("/usr/as/bin/" + path.stem + ".aex")
    else:
        # Only sources not yet migrated use the old source entry point. There
        # is no artifact-existence test here that could silently select it for A3.
        command = "as " + shlex.quote("/usr/as/examples/" + path.name)
    return command + (" &" if background else "")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("sources", "commands"))
    parser.add_argument("--background", action="append", default=[])
    parser.add_argument("sources", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.sources:
        if args.mode == "sources":
            if source_version(path) == 3:
                print(path.as_posix())
        else:
            print(guest_command(path, path.stem in args.background))


if __name__ == "__main__":
    main()
