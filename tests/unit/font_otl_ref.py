#!/usr/bin/env python3
"""Emit expected answers for the GSUB/GPOS/GDEF/kern access API in c/lib/text/otlayout.h.

The reference is fontTools' own binary decompiler, which reads the same bytes
through a completely separate implementation. The dump is a stream of QUESTIONS
with their ANSWERS -- "coverage index of glyph 412 in subtable 0 of lookup 7",
"the ligature that glyphs (12,13,14) form" -- so the C test calls the real API
and does not get to decide what it is being asked.

That shape matters because this API is somebody else's interface: a shaper is
going to trust otl_coverage_index and otl_gpos_pair the way it trusts a library,
and "the header exists" is not evidence.

Usage: font_otl_ref.py FONT OUT.bin
"""
import argparse
import struct
import sys

from fontTools.ttLib import TTFont

MAGIC = b"OTLQ"
VERSION = 1

(K_EOF, K_COUNTS, K_SCRIPT, K_LANGSYS, K_FEATURE, K_LOOKUP, K_SUBTYPE, K_COVIDX,
 K_CLASS, K_SINGLE, K_MULTI, K_LIG, K_PAIRV, K_KERN, K_GDEFC, K_SINGLEPOS,
 K_ALT) = range(17)

MAX_PER_SUBTABLE = 400          # keeps a 30k-glyph coverage from dominating


class Out:
    def __init__(self, fp):
        self.fp = fp
        self.n = 0

    def rec(self, kind, fmt, *vals):
        self.fp.write(struct.pack("<B", kind))
        if fmt:
            self.fp.write(struct.pack("<" + fmt, *vals))
        self.n += 1

    def raw(self, data):
        self.fp.write(data)


def tag_u32(t):
    b = (t + "    ")[:4].encode("latin-1")
    return struct.unpack(">I", b)[0]


def vr(v):
    """ValueRecord -> the four metrics, absent fields zero."""
    if v is None:
        return (0, 0, 0, 0)
    return (getattr(v, "XPlacement", 0) or 0, getattr(v, "YPlacement", 0) or 0,
            getattr(v, "XAdvance", 0) or 0, getattr(v, "YAdvance", 0) or 0)


def resolve_ext(st):
    """See through an Extension subtable; returns (effective_type, subtable)."""
    if hasattr(st, "ExtensionLookupType"):
        return st.ExtensionLookupType, st.ExtSubTable
    return None, st


def dump_table(out, font, which, tag):
    if tag not in font:
        return
    t = font[tag].table
    gid = font.getGlyphID

    scripts = t.ScriptList.ScriptRecord if t.ScriptList else []
    features = t.FeatureList.FeatureRecord if t.FeatureList else []
    lookups = t.LookupList.Lookup if t.LookupList else []
    out.rec(K_COUNTS, "BIII", which, len(scripts), len(features), len(lookups))

    for si, sr in enumerate(scripts):
        s = sr.Script
        lsr = s.LangSysRecord or []
        d = s.DefaultLangSys
        out.rec(K_SCRIPT, "BIIIiI", which, si, tag_u32(sr.ScriptTag), len(lsr),
                (d.ReqFeatureIndex if d and d.ReqFeatureIndex != 0xFFFF else -1),
                (len(d.FeatureIndex) if d else 0))
        for li, lr in enumerate(lsr):
            out.rec(K_LANGSYS, "BIII", which, si, li, tag_u32(lr.LangSysTag))

    for fi, fr in enumerate(features):
        idx = fr.Feature.LookupListIndex or []
        out.rec(K_FEATURE, "BIII", which, fi, tag_u32(fr.FeatureTag), len(idx))
        out.raw(struct.pack("<%dH" % len(idx), *idx))

    for li, lk in enumerate(lookups):
        subs = lk.SubTable or []
        out.rec(K_LOOKUP, "BIHHI", which, li, lk.LookupType, lk.LookupFlag, len(subs))
        for si, st in enumerate(subs):
            ext, real = resolve_ext(st)
            rtype = ext if ext is not None else lk.LookupType
            out.rec(K_SUBTYPE, "BIIH", which, li, si, rtype)
            dump_subtable(out, font, which, li, si, rtype, real, gid)


def cov_glyphs(cov):
    return list(cov.glyphs) if cov is not None else []


def dump_cov(out, which, li, si, cov, gid):
    gl = cov_glyphs(cov)
    step = max(1, len(gl) // MAX_PER_SUBTABLE)
    for i in range(0, len(gl), step):
        out.rec(K_COVIDX, "BIIHi", which, li, si, gid(gl[i]), i)


def dump_classdef(out, which, li, si, sel, cd, gid):
    if cd is None:
        return
    items = sorted(cd.classDefs.items())
    step = max(1, len(items) // MAX_PER_SUBTABLE)
    for i in range(0, len(items), step):
        name, cls = items[i]
        try:
            g = gid(name)
        except KeyError:
            continue
        out.rec(K_CLASS, "BIIBHi", which, li, si, sel, g, cls)


def dump_subtable(out, font, which, li, si, rtype, st, gid):
    if which == 0:      # GSUB
        if rtype == 1:
            dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
            items = sorted(st.mapping.items())
            step = max(1, len(items) // MAX_PER_SUBTABLE)
            for i in range(0, len(items), step):
                a, b = items[i]
                out.rec(K_SINGLE, "BIIHi", which, li, si, gid(a), gid(b))
        elif rtype == 2:
            dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
            items = sorted(st.mapping.items())[:MAX_PER_SUBTABLE]
            for a, seq in items:
                out.rec(K_MULTI, "BIIHB", which, li, si, gid(a), len(seq))
                out.raw(struct.pack("<%dH" % len(seq), *[gid(x) for x in seq]))
        elif rtype == 3:
            dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
            for a, alts in sorted(st.alternates.items())[:MAX_PER_SUBTABLE]:
                out.rec(K_ALT, "BIIHI", which, li, si, gid(a), len(alts))
        elif rtype == 4:
            dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
            n = 0
            for first, ligs in sorted(st.ligatures.items()):
                for lg in ligs:
                    comps = [gid(first)] + [gid(c) for c in lg.Component]
                    if len(comps) > 255:
                        continue
                    out.rec(K_LIG, "BIIB", which, li, si, len(comps))
                    out.raw(struct.pack("<%dHi" % len(comps), *(comps + [gid(lg.LigGlyph)])))
                    n += 1
                    if n >= MAX_PER_SUBTABLE:
                        return
        elif rtype in (5, 6):
            dump_context(out, which, li, si, st, gid, rtype == 6)
        elif rtype == 8:
            dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
            gl = cov_glyphs(getattr(st, "Coverage", None))
            subs = st.Substitute or []
            for i, g in enumerate(gl[:MAX_PER_SUBTABLE]):
                if i < len(subs):
                    out.rec(K_SINGLE, "BIIHi", which, li, si, gid(g), gid(subs[i]))
    else:               # GPOS
        if rtype == 1:
            dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
            gl = cov_glyphs(getattr(st, "Coverage", None))
            step = max(1, len(gl) // MAX_PER_SUBTABLE)
            for i in range(0, len(gl), step):
                if st.Format == 1:
                    v = st.Value
                else:
                    # ValueCount can be shorter than the coverage in fonts that
                    # trim it; those glyphs have no adjustment to check.
                    if i >= len(st.Value):
                        continue
                    v = st.Value[i]
                out.rec(K_SINGLEPOS, "BIIH4i", which, li, si, gid(gl[i]), *vr(v))
        elif rtype == 2:
            dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
            n = 0
            if st.Format == 1:
                gl = cov_glyphs(st.Coverage)
                for i, g in enumerate(gl):
                    if i >= len(st.PairSet):
                        break
                    for pvr in st.PairSet[i].PairValueRecord:
                        out.rec(K_PAIRV, "BIIHH8i", which, li, si, gid(g),
                                gid(pvr.SecondGlyph), *(vr(pvr.Value1) + vr(pvr.Value2)))
                        n += 1
                        if n >= MAX_PER_SUBTABLE:
                            return
            else:
                dump_classdef(out, which, li, si, 0, st.ClassDef1, gid)
                dump_classdef(out, which, li, si, 1, st.ClassDef2, gid)
                # Probe the class matrix through real glyph pairs, one
                # representative glyph per class.
                rep1, rep2 = {}, {}
                for name, c in st.ClassDef1.classDefs.items():
                    rep1.setdefault(c, name)
                for name, c in st.ClassDef2.classDefs.items():
                    rep2.setdefault(c, name)
                cov = set(cov_glyphs(st.Coverage))
                for c1 in range(min(st.Class1Count, 40)):
                    g1 = rep1.get(c1)
                    if c1 == 0:
                        g1 = next((g for g in cov
                                   if st.ClassDef1.classDefs.get(g, 0) == 0), None)
                    if g1 is None or g1 not in cov:
                        continue
                    for c2 in range(min(st.Class2Count, 40)):
                        g2 = rep2.get(c2)
                        if c2 == 0:
                            g2 = ".notdef"
                            if st.ClassDef2.classDefs.get(g2, 0) != 0:
                                continue
                        if g2 is None:
                            continue
                        cr = st.Class1Record[c1].Class2Record[c2]
                        out.rec(K_PAIRV, "BIIHH8i", which, li, si, gid(g1), gid(g2),
                                *(vr(cr.Value1) + vr(cr.Value2)))
                        n += 1
                        if n >= MAX_PER_SUBTABLE:
                            return
        elif rtype in (3, 4, 5, 6):
            dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
            dump_cov(out, which, li, si, getattr(st, "MarkCoverage", None), gid)
        elif rtype in (7, 8):
            dump_context(out, which, li, si, st, gid, rtype == 8)


def dump_context(out, which, li, si, st, gid, chain):
    """Contextual/chaining subtables: check the coverages and class defs, which is
    where the plumbing lives; the rule bodies are checked structurally by the C
    side against its own decode."""
    fmt = st.Format
    if fmt in (1, 2):
        dump_cov(out, which, li, si, getattr(st, "Coverage", None), gid)
    if fmt == 2:
        if chain:
            dump_classdef(out, which, li, si, 1, getattr(st, "BacktrackClassDef", None), gid)
            dump_classdef(out, which, li, si, 0, getattr(st, "InputClassDef", None), gid)
            dump_classdef(out, which, li, si, 2, getattr(st, "LookAheadClassDef", None), gid)
        else:
            dump_classdef(out, which, li, si, 0, getattr(st, "ClassDef", None), gid)


def dump_kern(out, font, gid):
    if "kern" not in font:
        return
    total = {}
    for sub in font["kern"].kernTables:
        if getattr(sub, "coverage", 1) & 0x0E:      # minimum / cross-stream / override
            continue
        if not (getattr(sub, "coverage", 1) & 1):   # not horizontal
            continue
        if getattr(sub, "format", 0) != 0:
            continue
        for (a, b), v in sub.kernTable.items():
            total[(a, b)] = total.get((a, b), 0) + v
    items = sorted(total.items())
    step = max(1, len(items) // 4000)
    for i in range(0, len(items), step):
        (a, b), v = items[i]
        try:
            out.rec(K_KERN, "HHi", gid(a), gid(b), v)
        except KeyError:
            pass


def dump_gdef(out, font, gid):
    if "GDEF" not in font:
        return
    t = font["GDEF"].table
    if t.GlyphClassDef is None:
        return
    items = sorted(t.GlyphClassDef.classDefs.items())
    step = max(1, len(items) // 4000)
    for i in range(0, len(items), step):
        name, c = items[i]
        try:
            out.rec(K_GDEFC, "Hi", gid(name), c)
        except KeyError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("font")
    ap.add_argument("out")
    args = ap.parse_args()

    font = TTFont(args.font, fontNumber=0)
    with open(args.out, "wb") as fp:
        out = Out(fp)
        fp.write(MAGIC)
        fp.write(struct.pack("<I", VERSION))
        dump_table(out, font, 0, "GSUB")
        dump_table(out, font, 1, "GPOS")
        dump_kern(out, font, font.getGlyphID)
        dump_gdef(out, font, font.getGlyphID)
        out.rec(K_EOF, "")
    print("font_otl_ref: %s -> %d records" % (args.font, out.n), file=sys.stderr)


if __name__ == "__main__":
    main()
