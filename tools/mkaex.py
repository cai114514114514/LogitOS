#!/usr/bin/env python3
"""Wrap an ELF64 in a 64-byte AEX header.
Usage: mkaex.py <in.elf> <out.aex> <name> [ext]

Header: magic[4]="AEX1", name[32], ext[8], flags(u32), reserved(u32), pad[12]."""
import sys

elf = sys.argv[1]
out = sys.argv[2]
name = sys.argv[3]
ext = sys.argv[4] if len(sys.argv) > 4 else ""

data = open(elf, "rb").read()
hdr = bytearray(64)
hdr[0:4] = b"AEX1"
nb = name.encode()[:31]
hdr[4:4 + len(nb)] = nb
eb = ext.encode()[:7]
hdr[36:36 + len(eb)] = eb
# flags @44, reserved @48, pad @52 stay zero

with open(out, "wb") as f:
    f.write(bytes(hdr) + data)
print(f"mkaex: {out}  name='{name}' ext='{ext}'  ({len(data)} ELF bytes)")
