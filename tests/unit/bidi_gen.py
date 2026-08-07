#!/usr/bin/env python3
"""Generate the Unicode property tables used by c/lib/text/{bidi,script,shape}.c.

These are the *only* Unicode properties the bidi/shaping line needs, and they are
generated into .inc files that are private to those three TUs -- this is not a
general Unicode-services module (UAX #14 line breaking and UAX #29 clusters are
someone else's table, deliberately not here).

Sources (Unicode Character Database, installed on the host as /usr/share/unicode):
    extracted/DerivedBidiClass.txt   Bidi_Class          -> bidi_data.inc
    BidiBrackets.txt                 Bidi_Paired_Bracket -> bidi_data.inc
    BidiMirroring.txt                Bidi_Mirroring_Glyph-> bidi_data.inc
    Scripts.txt                      Script              -> script_data.inc
    extracted/DerivedJoiningType.txt Joining_Type        -> script_data.inc
    extracted/DerivedCombiningClass.txt Canonical_Comb_Cl-> script_data.inc

Each property becomes a two-stage trie: stage1[cp >> 6] selects a deduplicated
64-byte block in stage2, so the common case (huge runs of one value) costs one
stage1 entry per 64 code points and nothing else.

Usage: bidi_gen.py [--ucd /usr/share/unicode] [--out c/lib/text]
"""
import argparse
import os
import sys

# ---------------------------------------------------------------- properties --

# UAX #9 Bidi_Class values, in the order c/lib/text/bidi.h declares them.
BIDI_CLASSES = ["L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN",
                "B", "S", "WS", "ON", "LRE", "LRO", "RLE", "RLO", "PDF",
                "LRI", "RLI", "FSI", "PDI"]

# Scripts the shaper distinguishes. Everything else collapses to SC_COMMON or
# SC_UNKNOWN, which is honest: we do not claim to shape what we do not name.
SCRIPTS = ["Unknown", "Common", "Inherited", "Latin", "Greek", "Cyrillic",
           "Arabic", "Hebrew", "Syriac", "Thaana", "Nko", "Devanagari",
           "Bengali", "Gurmukhi", "Gujarati", "Oriya", "Tamil", "Telugu",
           "Kannada", "Malayalam", "Sinhala", "Thai", "Lao", "Tibetan",
           "Myanmar", "Khmer", "Hangul", "Han", "Hiragana", "Katakana",
           "Armenian", "Georgian", "Ethiopic", "Mongolian"]

JOINING = ["U", "C", "D", "R", "L", "T"]   # Joining_Type; U = non-joining

# ------------------------------------------------------------------ parsing --

def parse_ranges(path, want_default=None):
    """Yield (first, last, value) from a UCD @-style property file."""
    out = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            # The @missing lines carry the property's default for unlisted code
            # points; Bidi_Class's default is *not* L for several blocks, so
            # ignoring them silently mis-classifies unassigned Arabic/Hebrew
            # space -- which BidiTest would then fail in a very confusing way.
            if line.startswith("# @missing:"):
                body = line.split(":", 1)[1].strip()
                rng, val = body.split(";")[0].strip(), body.split(";")[1].strip()
                lo, hi = (rng.split("..") + [rng])[:2]
                if want_default is not None:
                    want_default.append((int(lo, 16), int(hi, 16), val))
                continue
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            rng = parts[0]
            val = parts[1]
            lo, hi = (rng.split("..") + [rng])[:2]
            out.append((int(lo, 16), int(hi, 16), val))
    return out


def build_map(ranges, defaults, names, fallback):
    """Flatten ranges over the whole code space into a list of small ints."""
    idx = {n: i for i, n in enumerate(names)}
    arr = [idx[fallback]] * 0x110000
    for lo, hi, val in defaults:
        v = idx.get(val)
        if v is None:
            continue
        for cp in range(lo, min(hi, 0x10FFFF) + 1):
            arr[cp] = v
    for lo, hi, val in ranges:
        v = idx.get(val)
        if v is None:
            continue
        for cp in range(lo, min(hi, 0x10FFFF) + 1):
            arr[cp] = v
    return arr


# -------------------------------------------------------------------- tries --

# 256 is the measured minimum of (stage1 + deduplicated stage2) across the three
# properties: 101 KiB total, against 144 KiB at 64 and 110 KiB at 128. The tables
# land in the kernel image, so this is not a free choice.
BLOCK = 256

def make_trie(arr):
    """Two-stage trie. Returns (stage1, stage2) with stage2 in BLOCK chunks."""
    stage2 = []
    seen = {}
    stage1 = []
    for base in range(0, 0x110000, BLOCK):
        blk = tuple(arr[base:base + BLOCK])
        i = seen.get(blk)
        if i is None:
            i = len(stage2) // BLOCK
            seen[blk] = i
            stage2.extend(blk)
        stage1.append(i)
    return stage1, stage2


def emit_trie(fh, name, stage1, stage2, s1type="uint16_t", s2type="uint8_t"):
    assert max(stage1) < 65536, "stage1 overflowed uint16"
    fh.write("static const %s %s_s1[%d] = {\n" % (s1type, name, len(stage1)))
    for i in range(0, len(stage1), 16):
        fh.write("    " + ",".join(str(v) for v in stage1[i:i + 16]) + ",\n")
    fh.write("};\n")
    fh.write("static const %s %s_s2[%d] = {\n" % (s2type, name, len(stage2)))
    for i in range(0, len(stage2), 32):
        fh.write("    " + ",".join(str(v) for v in stage2[i:i + 32]) + ",\n")
    fh.write("};\n\n")


def trie_bytes(stage1, stage2):
    return len(stage1) * 2 + len(stage2)


# ------------------------------------------------------------------- output --

def gen_bidi(ucd, out):
    dflt = []
    ranges = parse_ranges(os.path.join(ucd, "extracted", "DerivedBidiClass.txt"), dflt)
    arr = build_map(ranges, dflt, BIDI_CLASSES, "L")
    s1, s2 = make_trie(arr)

    # Bidi_Paired_Bracket: BD16 needs the pair *and* its type (open/close).
    # Canonical equivalence matters for exactly two characters (U+2329/U+232A vs
    # U+3008/U+3009), which the spec calls out; we normalise them here so the
    # runtime does not need a decomposition table for that one case.
    canon = {0x2329: 0x3008, 0x232A: 0x3009}
    brackets = []
    with open(os.path.join(ucd, "BidiBrackets.txt"), encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            a, b, kind = [p.strip() for p in line.split(";")]
            cp, pair = int(a, 16), int(b, 16)
            brackets.append((cp, canon.get(pair, pair), 0 if kind == "o" else 1,
                             canon.get(cp, cp)))
    brackets.sort()

    mirror = []
    with open(os.path.join(ucd, "BidiMirroring.txt"), encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            a, b = [p.strip() for p in line.split(";")]
            mirror.append((int(a, 16), int(b, 16)))
    mirror.sort()

    path = os.path.join(out, "bidi_data.inc")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("/* GENERATED by tests/unit/bidi_gen.py -- do not edit.\n"
                 " * Bidi_Class, Bidi_Paired_Bracket and Bidi_Mirroring_Glyph\n"
                 " * from the Unicode Character Database. */\n\n")
        emit_trie(fh, "bidi_cls", s1, s2)
        fh.write("/* cp, paired cp, 0 = open / 1 = close. Sorted by cp. */\n")
        fh.write("struct bracket_ent { uint32_t cp, pair; uint8_t close; };\n")
        fh.write("static const struct bracket_ent bidi_brackets[%d] = {\n" % len(brackets))
        for cp, pair, close, _canon in brackets:
            fh.write("    {0x%04X,0x%04X,%d},\n" % (cp, pair, close))
        fh.write("};\n\n")
        fh.write("static const uint32_t bidi_mirror[%d][2] = {\n" % len(mirror))
        for a, b in mirror:
            fh.write("    {0x%04X,0x%04X},\n" % (a, b))
        fh.write("};\n")
    print("bidi_data.inc: trie %d bytes, %d bracket pairs, %d mirrors"
          % (trie_bytes(s1, s2), len(brackets), len(mirror)))


def gen_script(ucd, out):
    dflt = []
    sranges = parse_ranges(os.path.join(ucd, "Scripts.txt"), dflt)
    # Scripts.txt uses long names with underscores; ours match except Nko.
    fixed = []
    for lo, hi, v in sranges:
        v = {"Nko": "Nko"}.get(v, v)
        fixed.append((lo, hi, v))
    sarr = build_map(fixed, [], SCRIPTS, "Unknown")
    ss1, ss2 = make_trie(sarr)

    jdflt = []
    jranges = parse_ranges(os.path.join(ucd, "extracted", "DerivedJoiningType.txt"), jdflt)
    jarr = build_map(jranges, jdflt, JOINING, "U")
    js1, js2 = make_trie(jarr)

    path = os.path.join(out, "script_data.inc")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("/* GENERATED by tests/unit/bidi_gen.py -- do not edit.\n"
                 " * Script and Joining_Type from the Unicode Character Database. */\n\n")
        emit_trie(fh, "uscript", ss1, ss2)
        emit_trie(fh, "ujoin", js1, js2)
    print("script_data.inc: script trie %d bytes, joining trie %d bytes"
          % (trie_bytes(ss1, ss2), trie_bytes(js1, js2)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ucd", default="/usr/share/unicode")
    ap.add_argument("--out", default="c/lib/text")
    a = ap.parse_args()
    if not os.path.isdir(a.ucd):
        sys.exit("no UCD at %s" % a.ucd)
    gen_bidi(a.ucd, a.out)
    gen_script(a.ucd, a.out)


if __name__ == "__main__":
    main()
