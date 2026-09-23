#!/usr/bin/env python3
"""Build-only negative control: restore the two historical missing guards.

The source tree is never edited. Refuse a changed source pattern so a passing
control cannot silently exercise a different code path after refactoring.
"""

import argparse
from pathlib import Path


def mutate_slice(source: str) -> str:
    guarded = "if (sl->first_mb_in_slice < 0 || sl->first_mb_in_slice >= total)"
    old = "if (sl->first_mb_in_slice >= total)"
    if source.count(guarded) != 1:
        raise ValueError("H.264 slice guard changed; update the negative control")
    return source.replace(guarded, old, 1)


def mutate_crop(source: str) -> str:
    start = "        /* Crop units here are *2 luma samples"
    end = "    }\n    if (bs_u1(bs)) {"
    if source.count(start) != 1:
        raise ValueError("H.264 crop guard changed; update the negative control")
    first = source.index(start)
    last = source.index(end, first)
    guard = source[first:last]
    if "s.crop[i] < 0" not in guard or "s.crop[0] + s.crop[1]" not in guard:
        raise ValueError("H.264 crop guard is incomplete; update the control")
    return source[:first] + source[last:]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("kind", choices=("slice", "crop"))
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.source.read_text()
    changed = mutate_slice(source) if args.kind == "slice" else mutate_crop(source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(changed)


if __name__ == "__main__":
    main()
