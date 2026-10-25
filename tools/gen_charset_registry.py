#!/usr/bin/env python3
"""Generate charset identifiers from an explicitly supplied IANA CSV snapshot.

Only factual registry data is emitted. No network fetch happens during a build,
and neither removed NetSurf library is an input to this generator.
"""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path


def render(data):
    records = []
    seen = set()
    for row in csv.DictReader(io.StringIO(data.decode("utf-8-sig"))):
        number = row["MIBenum"].strip()
        if not number.isdecimal():
            continue
        mib = int(number)
        if not 0 < mib < 65536 or mib in seen:
            raise ValueError(f"invalid or repeated MIBenum: {mib}")
        seen.add(mib)
        name = (row["Preferred MIME Name"] or row["Name"]).strip()
        aliases = list(dict.fromkeys(
            alias.strip() for alias in [row["Name"], *row["Aliases"].splitlines()]
            if alias.strip() and alias.strip() != name))
        if any("|" in alias for alias in aliases):
            raise ValueError("an alias contains the output separator")
        records.append((mib, name, "|".join(aliases)))
    if len(records) < 200:
        raise ValueError("the input is not a complete IANA charset registry")
    digest = hashlib.sha256(data).hexdigest()
    lines = [
        "/* SPDX-License-Identifier: CC0-1.0",
        " * Generated registry DATA by tools/gen_charset_registry.py.",
        " * Source: https://www.iana.org/assignments/character-sets/character-sets-1.csv",
        f" * Input SHA-256: {digest}",
        " * IANA registry data: https://www.iana.org/help/licensing-terms",
        " * Query and conversion implementations are separate project code. */",
        '#include "registry.h"', "",
        "const charset_record charset_registry[] = {",
    ]
    for mib, name, aliases in sorted(records):
        lines.append(f"    {{{mib}, {json.dumps(name)}, {json.dumps(aliases)}}},")
    lines += ["};", "", "const size_t charset_registry_count =",
              "    sizeof(charset_registry) / sizeof(*charset_registry);", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = render(args.csv.read_bytes())
    if args.check:
        if args.output.read_text() != content:
            raise SystemExit("charset registry differs from the supplied snapshot")
        print("charset registry: snapshot and output agree")
    else:
        args.output.write_text(content)


if __name__ == "__main__":
    main()
