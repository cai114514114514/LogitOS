#!/usr/bin/env python3
"""Wrap an ELF64 in a 64-byte AEX v1 header.
Usage: mkaex.py <in.elf> <out.aex> <name> [ext] [icon] [r] [g] [b]

Header: magic[4]="AEX1", version(u16)=1, flags(u16), name[32], ext[8],
        icon(u8), icon_r/g/b(u8), pad[12]."""
import sys, struct

def u8arg(s, what):
    v = int(s)
    if not 0 <= v <= 255:
        sys.exit(f"mkaex: {what}={v} out of range 0..255")
    return v

elf  = sys.argv[1]
out  = sys.argv[2]
name = sys.argv[3]
ext  = sys.argv[4] if len(sys.argv) > 4 else ""
if ext == "-":                       # sentinel for "no extension"
    ext = ""
icon = sys.argv[5] if len(sys.argv) > 5 else ""
r    = u8arg(sys.argv[6], "r") if len(sys.argv) > 6 else 0
g    = u8arg(sys.argv[7], "g") if len(sys.argv) > 7 else 0
b    = u8arg(sys.argv[8], "b") if len(sys.argv) > 8 else 0

data = open(elf, "rb").read()
hdr = bytearray(64)
hdr[0:4] = b"AEX1"
struct.pack_into("<HH", hdr, 4, 1, 0)                 # version=1, flags=0
# Truncate to 31 bytes on a UTF-8 character boundary (decode+drop the partial
# tail sequence), never mid-codepoint.
nb = name.encode()[:31].decode("utf-8", "ignore").encode()
hdr[8:8 + len(nb)] = nb
eb = ext.encode()[:7]
hdr[40:40 + len(eb)] = eb
hdr[48] = ord(icon[0]) if icon else 0
hdr[49], hdr[50], hdr[51] = r, g, b

with open(out, "wb") as f:
    f.write(bytes(hdr) + data)
print(f"mkaex: {out}  name='{name}' ext='{ext}' icon='{icon}'  ({len(data)} ELF bytes)")
