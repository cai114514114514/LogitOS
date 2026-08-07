#!/usr/bin/env python3
"""Shape tests/unit/shape_corpus.txt with HarfBuzz and emit the expected output.

This is the reference side of the shaping differential. It is deliberately not
committed: the header goes into $(BUILD), so a HarfBuzz upgrade shows up as a
diff in the test result rather than silently becoming "our expected output".

For each corpus case it shapes every run separately -- with the script and
direction the corpus states -- and concatenates the runs in the visual order
the corpus states. tests/unit/shape_test.c then runs our whole pipeline (bidi,
segmentation, shaping) over the same text and must land on the same glyphs.

With --nogpos it also writes a GPOS-stripped copy of the font and a second
expectation table shaped with it. That is the only way to test the legacy
`kern` fallback differentially: a shaper reads `kern` only when the font has
no GPOS, and no font installed here is in that state -- so one is made.

Usage:
    shape_hb_gen.py --font FONT.ttf --corpus shape_corpus.txt --out expect.h
                    [--nogpos out/nogpos.ttf]
"""
import argparse
import os
import sys

try:
    import uharfbuzz as hb
except ImportError:
    sys.exit("shape_hb_gen.py: uharfbuzz is not installed in this interpreter\n"
             "  (tests/text.mk uses $(HBPY); install with "
             "'<HBPY> -m pip install uharfbuzz')")

# OpenType script tag (what the corpus writes, and what script_ot_tag returns)
# -> the ISO 15924 code HarfBuzz wants.
TAG_TO_ISO = {
    "latn": "Latn", "grek": "Grek", "cyrl": "Cyrl", "arab": "Arab",
    "hebr": "Hebr", "syrc": "Syrc", "thaa": "Thaa", "nko ": "Nkoo",
    "armn": "Armn", "geor": "Geor", "ethi": "Ethi", "thai": "Thai",
    "DFLT": "Zyyy",
}


def parse_corpus(path):
    cases = []
    with open(path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n").rstrip("\r")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 3:
                sys.exit("%s:%d: want 3 tab-separated fields, got %d"
                         % (path, lineno, len(parts)))
            name, text, runspec = parts
            runs = []
            for r in runspec.split(";"):
                f = r.split(":")
                if len(f) != 4:
                    sys.exit("%s:%d: bad run %r" % (path, lineno, r))
                tag, d, start, ln = f[0], f[1], int(f[2]), int(f[3])
                if tag not in TAG_TO_ISO:
                    sys.exit("%s:%d: unknown script tag %r" % (path, lineno, tag))
                if d not in ("L", "R"):
                    sys.exit("%s:%d: direction must be L or R" % (path, lineno))
                runs.append((tag, d == "R", start, ln))
            # The runs must tile the text exactly: no gap, no overlap, nothing
            # past the end. This is where a hand-written index typo dies.
            covered = [0] * len(text)
            for _tag, _rtl, start, ln in runs:
                if start < 0 or ln <= 0 or start + ln > len(text):
                    sys.exit("%s:%d: run %d:%d is outside the %d code point text"
                             % (path, lineno, start, ln, len(text)))
                for i in range(start, start + ln):
                    covered[i] += 1
            if any(c != 1 for c in covered):
                sys.exit("%s:%d: runs do not tile the text exactly (coverage %r)"
                         % (path, lineno, covered))
            cases.append((name, text, runs))
    return cases


def shape_run(font, text, tag, rtl):
    buf = hb.Buffer()
    buf.add_str(text)
    buf.direction = "rtl" if rtl else "ltr"
    buf.script = TAG_TO_ISO[tag]
    buf.language = ""
    hb.shape(font, buf)
    out = []
    for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
        out.append((info.codepoint, pos.x_advance, pos.y_advance,
                    pos.x_offset, pos.y_offset))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--nogpos", help="write a GPOS-stripped copy of --font here "
                                     "and emit a second expectation table for it")
    a = ap.parse_args()

    if not os.path.exists(a.font):
        sys.exit("shape_hb_gen.py: no font at %s" % a.font)

    blob = hb.Blob.from_file_path(a.font)
    face = hb.Face(blob)
    font = hb.Font(face)
    # Font units, not pixels: our shaper works in font units and the test
    # scales at the very end, so the reference has to be unscaled too.
    font.scale = (face.upem, face.upem)

    cases = parse_corpus(a.corpus)

    # The legacy `kern` table is a FALLBACK: a shaper only reads it when the
    # font has no GPOS, which no font on this machine is. So make one. Stripping
    # GPOS from the corpus font leaves its real `kern` table as the only source
    # of kerning, for HarfBuzz and for us alike -- which is the only way to
    # differentially test a path that otherwise has no font to run on.
    kern_font = None
    if a.nogpos:
        from fontTools.ttLib import TTFont
        tt = TTFont(a.font)
        if "GPOS" in tt:
            del tt["GPOS"]
        if "kern" not in tt:
            sys.exit("shape_hb_gen.py: %s has no legacy kern table to fall back to"
                     % a.font)
        tt.save(a.nogpos)
        kb = hb.Blob.from_file_path(a.nogpos)
        kf = hb.Face(kb)
        kern_font = hb.Font(kf)
        kern_font.scale = (kf.upem, kf.upem)

    with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("/* GENERATED by tests/unit/shape_hb_gen.py -- do not edit, do not commit.\n"
                 " * font:      %s\n"
                 " * corpus:    %s\n"
                 " * harfbuzz:  %s (uharfbuzz %s)\n"
                 " */\n\n"
                 % (a.font, a.corpus, hb.HARFBUZZ_VERSION_STRING
                    if hasattr(hb, "HARFBUZZ_VERSION_STRING") else "?",
                    getattr(hb, "__version__", "?")))
        fh.write("#define SHAPE_EXPECT_UPEM %d\n" % face.upem)
        fh.write("#define SHAPE_EXPECT_FONT \"%s\"\n\n" % a.font)
        fh.write("struct sx_glyph { unsigned gid; int xa, ya, xo, yo; };\n"
                 "struct sx_run   { const char *tag; int rtl, start, len; };\n"
                 "struct sx_case  {\n"
                 "    const char *name;\n"
                 "    const unsigned *cps; int ncp;\n"
                 "    const struct sx_run *runs; int nrun;\n"
                 "    const struct sx_glyph *glyphs; int nglyph;\n"
                 "};\n\n")

        total_glyphs = 0
        for idx, (name, text, runs) in enumerate(cases):
            cps = [ord(ch) for ch in text]
            glyphs = []
            for tag, rtl, start, ln in runs:
                glyphs += shape_run(font, text[start:start + ln], tag, rtl)
            total_glyphs += len(glyphs)

            fh.write("/* %s: %d code points, %d runs, %d glyphs */\n"
                     % (name, len(cps), len(runs), len(glyphs)))
            fh.write("static const unsigned sx_cps_%d[] = {%s};\n"
                     % (idx, ",".join("0x%04X" % c for c in cps)))
            fh.write("static const struct sx_run sx_runs_%d[] = {%s};\n"
                     % (idx, ",".join('{"%s",%d,%d,%d}' % (t, 1 if r else 0, s, l)
                                      for t, r, s, l in runs)))
            fh.write("static const struct sx_glyph sx_g_%d[] = {%s};\n\n"
                     % (idx, ",".join("{%d,%d,%d,%d,%d}" % g for g in glyphs)))

        # The legacy-kern reference, shaped with the GPOS-stripped font.
        kern_cases = []
        if kern_font is not None:
            for idx, (name, text, runs) in enumerate(cases):
                if not name.startswith("latin-"):
                    continue
                glyphs = []
                for tag, rtl, start, ln in runs:
                    glyphs += shape_run(kern_font, text[start:start + ln], tag, rtl)
                ki = len(kern_cases)
                fh.write("/* legacy kern: %s, %d glyphs */\n" % (name, len(glyphs)))
                fh.write("static const struct sx_glyph sx_kg_%d[] = {%s};\n"
                         % (ki, ",".join("{%d,%d,%d,%d,%d}" % g for g in glyphs)))
                kern_cases.append((name, idx, len(glyphs)))
            fh.write("#define SHAPE_EXPECT_NOGPOS \"%s\"\n" % a.nogpos)

        fh.write("static const struct sx_case shape_expect[] = {\n")
        for idx, (name, text, runs) in enumerate(cases):
            fh.write('    {"%s", sx_cps_%d, %d, sx_runs_%d, %d, sx_g_%d, '
                     '(int)(sizeof sx_g_%d / sizeof sx_g_%d[0])},\n'
                     % (name, idx, len(text), idx, len(runs), idx, idx, idx))
        fh.write("};\n")
        fh.write("#define SHAPE_EXPECT_N (int)(sizeof shape_expect / sizeof shape_expect[0])\n")
        if kern_cases:
            fh.write("static const struct sx_case shape_kern_expect[] = {\n")
            for ki, (name, idx, ng) in enumerate(kern_cases):
                fh.write('    {"%s", sx_cps_%d, %d, sx_runs_%d, %d, sx_kg_%d, %d},\n'
                         % (name, idx, len(cases[idx][1]), idx,
                            len(cases[idx][2]), ki, ng))
            fh.write("};\n")
            fh.write("#define SHAPE_KERN_N (int)(sizeof shape_kern_expect "
                     "/ sizeof shape_kern_expect[0])\n")

    print("shape_hb_gen.py: %d cases, %d runs, %d reference glyphs from %s"
          % (len(cases), sum(len(r) for _n, _t, r in cases), total_glyphs,
             os.path.basename(a.font)))
    if kern_font is not None:
        print("shape_hb_gen.py: %d legacy-kern cases from %s (GPOS stripped)"
              % (len(kern_cases), os.path.basename(a.nogpos)))


if __name__ == "__main__":
    main()
