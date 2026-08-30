#!/usr/bin/env python3
"""Assert the vertical-metrics identity between each Regular face and its
Bold twin in fsroot/fonts.

    python3 tests/fixtures/bold/check_font_metrics.py [fonts-dir]
    python3 tests/fixtures/bold/check_font_metrics.py --corrupt-ascent FILE

fsroot/fonts/README.md states the rule this gate enforces:

    c/lib/text/ttf.c reads `hhea` for ascent/descent/lineGap, and
    c/kernel/gui/text.c puts the baseline at `ascent * px / upem` using the
    FIRST font of the run's fallback set. A Bold face whose `hhea.ascent`
    differed from its Regular twin's would therefore shift every bold line
    vertically against the regular text beside it.

and records the measured pair -- ui: 1160/-288/0, mono: 1069/-293/0 -- with
the warning "nothing else in the tree asserts it". Until this gate, that was
true twice over: tests/unit/font_weight_test.c does carry a "line height is
the SAME for both weights" check, but it routes through text_line_height(),
which in c/kernel/gui/text.c only ever reads F_UI/F_MONO -- the REGULAR
faces -- so the check compares the same four numbers with themselves and
cannot fail on any bold-face drift. A control that cannot fail is worse than
no control, because it reads like one. This gate reads the hhea/head tables
of the actual bytes on the disk, so a one-unit drift in either twin reddens
it by name.

Why absolute numbers and not just twin-equality: equality alone would let a
regeneration move the PAIR together (an upstream source swap, an instancer
change) and every recorded pixel baseline that assumes a 1.16-em ui ascent
would shift silently. The absolute values are the README's; if a legitimate
regeneration ever changes them, this gate fails loudly and the README, this
table and the baselines get updated in the same commit -- which is the point
of writing the numbers down.

Parsing is done here rather than through c/lib/text/ttf.c on purpose: the
property under test is a fact about the FONT FILES (the README calls it "a
property of the sources, not of this pipeline"), and asserting it through
the same code that consumes it would let a ttf.c bug hide an asset drift --
or an asset drift hide a ttf.c bug. Two independent readers, one oracle
each. The tables read are tiny and stable: sfnt table directory at offset
12 (16-byte records), hhea ascender/descender/lineGap at +4/+6/+8, head
unitsPerEm at +18. No dependency on fontTools, so the gate cannot be
skipped for a missing host module.

--corrupt-ascent FILE bumps the hhea ascender of FILE by one unit, in place,
and exits. It exists ONLY as the negative control's sabotage (see
test-bold-metrics-negctl in tests/bold.mk): the control must watch THIS
gate go red on the smallest drift a bold face could really acquire.
"""
import struct
import sys
import os

# The README's recorded values: (ascender, descender, lineGap) per family.
# upem is asserted equal across the pair and recorded from measurement
# (2026-08-30, both families ship unitsPerEm=1000); the README's sentence
# names ascent/descent/lineGap only.
EXPECT = {
    "ui":   {"asc": 1160, "desc": -288, "gap": 0},
    "mono": {"asc": 1069, "desc": -293, "gap": 0},
}


def read_tables(path):
    """Return (ascender, descender, lineGap, unitsPerEm) for a TTF."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 12:
        die("%s: too short to be a font" % path)
    ver, num_tables = struct.unpack_from(">IH", data, 0)
    if ver != 0x00010000:
        die("%s: not a plain TrueType sfnt (version 0x%08x)" % (path, ver))
    hhea_off = head_off = None
    for i in range(num_tables):
        tag, _chk, off, _len = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        if tag == b"hhea":
            hhea_off = off
        elif tag == b"head":
            head_off = off
    if hhea_off is None or head_off is None:
        die("%s: no hhea or no head table" % path)
    asc, desc, gap = struct.unpack_from(">hhh", data, hhea_off + 4)
    (upem,) = struct.unpack_from(">H", data, head_off + 18)
    return asc, desc, gap, upem


def die(msg):
    print("FAIL  %s" % msg)
    sys.exit(1)


def corrupt_ascent(path):
    """The negative control's sabotage: ascender += 1, in place."""
    with open(path, "rb") as fh:
        data = bytearray(fh.read())
    ver, num_tables = struct.unpack_from(">IH", data, 0)
    if ver != 0x00010000:
        die("%s: not a plain TrueType sfnt" % path)
    hhea_off = None
    for i in range(num_tables):
        tag, _chk, off, _len = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        if tag == b"hhea":
            hhea_off = off
    if hhea_off is None:
        die("%s: no hhea table" % path)
    (asc,) = struct.unpack_from(">h", data, hhea_off + 4)
    struct.pack_into(">h", data, hhea_off + 4, asc + 1)
    with open(path, "wb") as fh:
        fh.write(data)
    print("sabotaged %s: hhea.ascender %d -> %d" % (path, asc, asc + 1))
    return 0


def main():
    args = sys.argv[1:]
    if args and args[0] == "--corrupt-ascent":
        if len(args) != 2:
            print("usage: check_font_metrics.py --corrupt-ascent FILE")
            return 2
        return corrupt_ascent(args[1])

    root = args[0] if args else "fsroot/fonts"
    fails = 0
    rows = []
    for fam in ("ui", "mono"):
        vals = {}
        for suffix, label in (("", "regular"), ("-bold", "bold")):
            path = os.path.join(root, "%s%s.ttf" % (fam, suffix))
            if not os.path.exists(path):
                print("FAIL  %s is missing -- nothing below is checkable" % path)
                fails += 1
                vals[label] = None
                continue
            vals[label] = read_tables(path)
        reg, bold = vals["regular"], vals["bold"]
        for label, v in (("regular", reg), ("bold", bold)):
            if v:
                rows.append("%-14s asc=%-5d desc=%-5d lineGap=%-3d upem=%d"
                            % ("%s %s" % (fam, label), v[0], v[1], v[2], v[3]))
        if not reg or not bold:
            continue
        exp = EXPECT[fam]
        # Twin identity -- the README's rule, checked field by field so the
        # failure names the drifted field.
        for idx, field in ((0, "ascent"), (1, "descent"), (2, "lineGap"),
                           (3, "unitsPerEm")):
            if reg[idx] != bold[idx]:
                print("FAIL  %s: %s differs between the twins -- regular %d,"
                      " bold %d. Every bold line would shift vertically"
                      " against the regular text beside it (see"
                      " fsroot/fonts/README.md)."
                      % (fam, field, reg[idx], bold[idx]))
                fails += 1
        # Absolute values -- the numbers the README measured.
        for idx, field in ((0, "asc"), (1, "desc"), (2, "gap")):
            if reg[idx] != exp[field]:
                print("FAIL  %s regular: hhea %s is %d, the recorded value is"
                      " %d -- either the fonts were regenerated and this gate"
                      " plus fsroot/fonts/README.md must be updated together,"
                      " or the wrong file is on the disk."
                      % (fam, field, reg[idx], exp[field]))
                fails += 1

    for r in rows:
        print("  " + r)
    if fails:
        print("check_font_metrics: FAIL (%d)" % fails)
        return 1
    print("check_font_metrics: ALL PASS -- both bold twins carry their"
          " regular twin's vertical metrics exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
