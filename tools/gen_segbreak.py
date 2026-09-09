#!/usr/bin/env python3
"""Generate the UAX #29 segmentation tables used by c/apps/browser/js_intl.c
(Intl.Segmenter).

WHY A SECOND GENERATED TABLE FILE BESIDE linebreak_data.inc.  That one is
UAX #14 (Line_Break) for layout_text.c; this one is UAX #29
(Grapheme_Cluster_Break + Word_Break + the Extended_Pictographic flag the
GB11/WB3c emoji rules test) for js_intl.c.  Same Unicode revision (16.0.0)
so the tree speaks one Unicode, but the properties are disjoint and the
consumers are different binaries' concerns -- merging them would make the
line-break file grow for a reason it cannot state.

SOURCES.  The tree's other generators read /usr/share/unicode, but this host
(macOS) has no such directory, so the needed UCD 16.0.0 files are COMMITTED
under tests/fixtures/intlseg/ucd16/ (fetched once, host-side, 2026-08-30):

    GraphemeBreakProperty.txt   Grapheme_Cluster_Break
    WordBreakProperty.txt       Word_Break
    emoji/emoji-data.txt        Extended_Pictographic

--ucd still exists and accepts a full UCD checkout for regeneration; the
fixture directory is the default so the gate can rebuild without network.
Both must be the SAME Unicode revision as these fixtures, or the .inc and
the committed test vectors (GraphemeBreakTest.txt / WordBreakTest.txt, also
under that fixture directory) stop describing the same standard.

The output is c/apps/browser/segbreak_data.inc: two deduplicated two-stage
tries (one per property) plus one for Extended_Pictographic, and the class
enums -- emitted here, not written by hand in js_intl.c, so the enum order
and the table cannot drift apart (same discipline as linebreak_data.inc).

NO FOLDINGS are applied here, unlike linebreak_gen.py's LB1.  UAX #29 has no
resolution step: Other (the default) is the only implicit class, and every
rule's exceptions are stated over the raw classes, so the raw classes are
what the engine in js_intl.c wants.

Usage: gen_segbreak.py [--ucd tests/fixtures/intlseg/ucd16] [--out c/apps/browser]
"""
import argparse
import os
import re
import sys

# --------------------------------------------------------------- classes ----

# Every Grapheme_Cluster_Break value plus the implicit Other.  The order IS
# the enum order emitted into the .inc; Other is 0 so a zeroed table entry
# (an unassigned code point) reads as the safe default.
GCB_CLASSES = [
    "Other", "CR", "Control", "Extend", "L", "LF", "LV", "LVT",
    "Prepend", "Regional_Indicator", "SpacingMark", "T", "V", "ZWJ",
]

# Every Word_Break value plus the implicit Other.  ExtendNumLet is the
# underscore class; WSegSpace is the run-of-spaces rule WB3d.
WB_CLASSES = [
    "Other", "ALetter", "CR", "Double_Quote", "Extend", "ExtendNumLet",
    "Format", "Hebrew_Letter", "Katakana", "LF", "MidLetter", "MidNum",
    "MidNumLet", "Newline", "Numeric", "Regional_Indicator", "Single_Quote",
    "WSegSpace", "ZWJ",
]

# --------------------------------------------------------------- parsing ----


def parse_ranges(path):
    """Yield (first, last, value) from a UCD @-style property file."""
    out = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            rng = parts[0]
            lo, hi = (rng.split("..") + [rng])[:2]
            out.append((int(lo, 16), int(hi, 16), parts[1]))
    return out


def flatten(ranges, fallback, ncp=0x110000):
    """Flatten (lo, hi, value) ranges over the whole code space."""
    arr = [fallback] * ncp
    for lo, hi, val in ranges:
        for cp in range(lo, min(hi, 0x10FFFF) + 1):
            arr[cp] = val
    return arr


def extpict_set(path):
    out = set()
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            rng, prop = [p.strip() for p in line.split(";")[:2]]
            if prop != "Extended_Pictographic":
                continue
            lo, hi = (rng.split("..") + [rng])[:2]
            out.update(range(int(lo, 16), int(hi, 16) + 1))
    return out


def ucd_version(ucd):
    """Read the revision out of one of the files' own headers."""
    with open(os.path.join(ucd, "GraphemeBreakProperty.txt"),
              encoding="utf-8") as fh:
        head = fh.read(4000)
    m = re.search(r"#\s*(\d+\.\d+\.\d+)", head)
    return m.group(1) if m else "?"


# ------------------------------------------------------------------ tries ---
# Same two-stage scheme as tools/linebreak_gen.py (256-code-point blocks,
# stage1 uint16 indices, stage2 uint8 values), copied rather than imported:
# tools/ is not a package and a shared helper would couple this generator to
# that file's internals for ~30 lines.

BLOCK = 256


def make_trie(arr):
    stage2, seen, stage1 = [], {}, []
    for base in range(0, 0x110000, BLOCK):
        blk = tuple(arr[base:base + BLOCK])
        i = seen.get(blk)
        if i is None:
            i = len(stage2) // BLOCK
            seen[blk] = i
            stage2.extend(blk)
        stage1.append(i)
    return stage1, stage2


def emit_trie(fh, name, stage1, stage2):
    assert max(stage1) < 65536, "stage1 overflowed uint16"
    assert max(stage2) < 256, "stage2 overflowed uint8"
    fh.write("static const uint16_t %s_s1[%d] = {\n" % (name, len(stage1)))
    for i in range(0, len(stage1), 16):
        fh.write("    " + ",".join(str(v) for v in stage1[i:i + 16]) + ",\n")
    fh.write("};\n")
    fh.write("static const uint8_t %s_s2[%d] = {\n" % (name, len(stage2)))
    for i in range(0, len(stage2), 32):
        fh.write("    " + ",".join(str(v) for v in stage2[i:i + 32]) + ",\n")
    fh.write("};\n\n")


# ----------------------------------------------------------------- output ---

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ucd", default="tests/fixtures/intlseg/ucd16")
    ap.add_argument("--out", default="c/apps/browser")
    a = ap.parse_args()
    if not os.path.isdir(a.ucd):
        sys.exit("no UCD at %s" % a.ucd)

    gcb = flatten(parse_ranges(os.path.join(a.ucd, "GraphemeBreakProperty.txt")),
                  "Other")
    wb = flatten(parse_ranges(os.path.join(a.ucd, "WordBreakProperty.txt")),
                 "Other")
    ext = extpict_set(os.path.join(a.ucd, "emoji-data.txt"))

    gi = {n: i for i, n in enumerate(GCB_CLASSES)}
    wi = {n: i for i, n in enumerate(WB_CLASSES)}
    gcb_cls = [gi[v] for v in gcb]
    wb_cls = [wi[v] for v in wb]
    ep = [1 if cp in ext else 0 for cp in range(0x110000)]

    gs1, gs2 = make_trie(gcb_cls)
    ws1, ws2 = make_trie(wb_cls)
    es1, es2 = make_trie(ep)

    path = os.path.join(a.out, "segbreak_data.inc")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("/* GENERATED by tools/gen_segbreak.py -- do not edit.\n"
                 " * Grapheme_Cluster_Break and Word_Break (UAX #29), plus the\n"
                 " * Extended_Pictographic flag rules GB11/WB3c test, from the\n"
                 " * UCD files committed under tests/fixtures/intlseg/ucd16.\n"
                 " * Unicode %s. */\n\n" % ucd_version(a.ucd))
        for title, classes, pfx in (("Grapheme_Cluster_Break", GCB_CLASSES, "GCB"),
                                    ("Word_Break", WB_CLASSES, "WB")):
            fh.write("/* %s classes. Order is the table's vocabulary; do not\n"
                     " * reorder without regenerating. */\n" % title)
            fh.write("enum {\n")
            for i, n in enumerate(classes):
                fh.write("    SEG_%s_%s = %d,\n" % (pfx, n.upper(), i))
            fh.write("};\n\n")
        emit_trie(fh, "seg_gcb", gs1, gs2)
        emit_trie(fh, "seg_wb", ws1, ws2)
        emit_trie(fh, "seg_extpict", es1, es2)
        fh.write("/* 1 iff Extended_Pictographic (emoji/emoji-data.txt). The\n"
                 " * grapheme rule GB11 and the word rule WB3c both need it as\n"
                 " * a fact SEPARATE from the class tables: an Extend that is\n"
                 " * pictographic and one that is not break differently. */\n"
                 "static int seg_is_extpict(uint32_t cp) {\n"
                 "    return seg_extpict_s2[((uint32_t)seg_extpict_s1[cp >> 8] << 8)\n"
                 "                          | (cp & 0xff)] != 0;\n"
                 "}\n\n")
        fh.write("static int seg_gcb_class(uint32_t cp) {\n"
                 "    return seg_gcb_s2[((uint32_t)seg_gcb_s1[cp >> 8] << 8)\n"
                 "                      | (cp & 0xff)];\n"
                 "}\n\n")
        fh.write("static int seg_wb_class(uint32_t cp) {\n"
                 "    return seg_wb_s2[((uint32_t)seg_wb_s1[cp >> 8] << 8)\n"
                 "                      | (cp & 0xff)];\n"
                 "}\n")
    print("wrote %s: gcb %d B, wb %d B, extpict %d B (Unicode %s)"
          % (path, len(gs1) * 2 + len(gs2), len(ws1) * 2 + len(ws2),
             len(es1) * 2 + len(es2), ucd_version(a.ucd)))


if __name__ == "__main__":
    main()
