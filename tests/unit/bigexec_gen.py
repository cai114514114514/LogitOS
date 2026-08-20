#!/usr/bin/env python3
"""Generate the pad blob bigexec_pad.S embeds.

Deterministic (a fixed seed) so the same size always produces the same bytes and
two runs are comparable, and INCOMPRESSIBLE so nothing between here and the
loader -- the linker, mkaex, the filesystem -- can elide the section and leave
the test measuring a hole.

Three bytes are planted at the positions bigexec_pad.c reads, so the program's
printed `sum=` is a check on the CONTENTS. A loader that maps the right number
of pages holding the wrong bytes (the file-run trimming being off by a page, a
segment read from the wrong offset) prints a wrong sum rather than crashing,
and a crash is the failure mode that is easy to notice anyway.

usage: bigexec_gen.py <bytes> <out.bin>
"""
import sys, random

n = int(sys.argv[1])
out = sys.argv[2]
if n < 3:
    sys.exit("bigexec_gen: a pad smaller than 3 bytes has no middle")

buf = bytearray(random.Random(0x10517).randbytes(n))
buf[-1] = 0xA5          # the byte main() touches LAST
buf[n // 2] = 0x5A
buf[0] = 0x3C
# 0xA5 + 0x5A + 0x3C = 0x13B = 315, and that is the number the harness greps.
with open(out, "wb") as f:
    f.write(bytes(buf))
print("bigexec_gen: %s %d bytes, sum of the three probes = %d"
      % (out, n, buf[-1] + buf[n // 2] + buf[0]))
