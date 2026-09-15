#!/usr/bin/env python3
"""genmpeg4_tables.py -- generate c/lib/video/mpeg4_tables.h from reference source.

THE TABLES ARE NOT TYPED.  MPEG-4 Part 2 and H.263 code every macroblock through
variable-length codes whose lengths run to fifteen bits; one wrong entry does not
shade a pixel, it desynchronises the bit reader and the rest of the picture is
noise.  Three thousand hand-copied hex digits are not checkable by looking, so
they are lifted mechanically out of a reference implementation instead --
exactly the argument tools/gen_vp8_tables.py makes for VP8's probabilities.

SOURCE: the FFmpeg tree at tag n7.1, files

    libavcodec/h263data.c        H.263 MCBPC / CBPY / MVD / TCOEF
    libavcodec/mpeg4data.h       MPEG-4 DC / intra TCOEF / RVLC / matrices
    libavcodec/mathtables.c      ff_zigzag_direct
    libavcodec/mpegvideodata.c   the two alternate scans

These are the same arrays ISO/IEC 14496-2 Annex B and ITU-T H.263 Tables 12-16
print; FFmpeg is used as the machine-readable copy of them, and it is also the
oracle tests/mpeg4.mk diffs against, so a disagreement between the two would be
caught by the decode gate rather than hidden by it.

USAGE
    genmpeg4_tables.py --src <ffmpeg-source-dir> [-o c/lib/video/mpeg4_tables.h]
    genmpeg4_tables.py --src <ffmpeg-source-dir> --check [-o ...]

--check regenerates in memory and compares byte for byte against the committed
header: run against an unmodified source it must reproduce it exactly, which is
what makes a diff of the generated file readable.  Every table's shape and
CRC-32 are printed, and every prefix code is checked for ambiguity (two codes
where one is a prefix of the other) before anything is written -- a check the
Kraft sum alone does not make.
"""

import argparse
import os
import re
import sys
import zlib

# ---------------------------------------------------------------- C parsing --

def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def find_array(text, name, expect=None):
    """Extract every integer literal from the initialiser of `name`.

    Refuses rather than guesses: the declaration must be a real definition
    (`... name [ ... ] = { ... };`), and when `expect` is given the count must
    match exactly.  An array that also appears as a *function argument*
    elsewhere in the file is not a declaration and is not matched, because the
    pattern requires the `=` and the brace.
    """
    pat = re.compile(r"\b" + re.escape(name) + r"\s*(\[[^;=]*\])+\s*=\s*\{", re.S)
    m = pat.search(text)
    if not m:
        raise SystemExit("genmpeg4_tables: no definition of %s" % name)
    if pat.search(text, m.end()):
        raise SystemExit("genmpeg4_tables: %s defined more than once" % name)
    i = m.end() - 1
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                body = text[i + 1:j]
                break
    else:
        raise SystemExit("genmpeg4_tables: unterminated initialiser for %s" % name)
    vals = [int(v, 0) for v in re.findall(r"-?\b(?:0[xX][0-9a-fA-F]+|\d+)\b", body)]
    if expect is not None and len(vals) != expect:
        raise SystemExit("genmpeg4_tables: %s has %d values, expected %d"
                         % (name, len(vals), expect))
    return vals


def pairs(vals):
    return [(vals[i], vals[i + 1]) for i in range(0, len(vals), 2)]


# ------------------------------------------------------------ VLC machinery --

class Vlc:
    """A prefix code as the decoder consumes it: entries sorted by (len, code).

    The decoder shifts one bit at a time and binary-searches the entries of the
    current length.  No lookup table is materialised -- a 15-bit RVLC table
    would cost 64 KiB of rodata in every ring-3 binary that links c/lib/video,
    and this decoder is held to bit-exactness, not to throughput.
    """

    def __init__(self, name, entries):
        # entries: list of (code, length, symbol)
        self.name = name
        self.entries = sorted(entries, key=lambda e: (e[1], e[0]))
        self.maxlen = max(e[1] for e in entries)
        self.minlen = min(e[1] for e in entries)
        if self.maxlen > 16:
            raise SystemExit("%s: code longer than 16 bits" % name)
        self.check_prefix_free()

    def check_prefix_free(self):
        seen = {}
        for code, ln, sym in self.entries:
            if (code, ln) in seen:
                raise SystemExit("%s: duplicate code %0*x/%d"
                                 % (self.name, (ln + 3) // 4, code, ln))
            seen[(code, ln)] = sym
        for code, ln, _ in self.entries:
            for shorter in range(self.minlen, ln):
                if (code >> (ln - shorter), shorter) in seen:
                    raise SystemExit(
                        "%s: code %0*x/%d has the %d-bit prefix %x"
                        % (self.name, (ln + 3) // 4, code, ln, shorter,
                           code >> (ln - shorter)))

    def offsets(self):
        off = [0] * (self.maxlen + 2)
        cnt = [0] * (self.maxlen + 2)
        for _, ln, _ in self.entries:
            cnt[ln] += 1
        run = 0
        for ln in range(self.maxlen + 2):
            off[ln] = run
            run += cnt[ln]
        return off, cnt

    def kraft(self):
        return sum(2.0 ** -ln for _, ln, _ in self.entries)


MAX_RUN = 64
MAX_LEVEL = 64


class RlTable:
    """An MPEG-4/H.263 run-level VLC plus the two escape helper tables.

    max_level[last][run] and max_run[last][level] are what escape types 1 and 2
    add to the value read back through the same VLC (14496-2 7.4.1.3).  They are
    derived here from the (run, level, last) triples rather than stated, so they
    cannot disagree with the table they describe.
    """

    def __init__(self, name, vlc_pairs, runs, levels, last_index):
        n = len(runs)
        if len(levels) != n or len(vlc_pairs) != n + 1:
            raise SystemExit("%s: shape mismatch (%d/%d/%d)"
                             % (name, len(vlc_pairs), n, len(levels)))
        self.name = name
        self.n = n
        self.last_index = last_index
        self.runs = runs
        self.levels = levels
        self.escape = vlc_pairs[n]          # the ESC codeword, symbol == n
        entries = [(vlc_pairs[i][0], vlc_pairs[i][1], i) for i in range(n + 1)]
        # The prefix code gets the "_vlc" suffix so the m4_rl struct below can
        # point at it by name without colliding with the m4_rl object itself.
        self.vlc = Vlc(name + "_vlc", entries)
        self.max_level = [[0] * (MAX_RUN + 1) for _ in range(2)]
        self.max_run = [[0] * (MAX_LEVEL + 1) for _ in range(2)]
        for last in range(2):
            start, end = (0, last_index) if last == 0 else (last_index, n)
            for i in range(start, end):
                r, l = runs[i], levels[i]
                if r > MAX_RUN or l > MAX_LEVEL:
                    raise SystemExit("%s: run/level out of range" % name)
                if l > self.max_level[last][r]:
                    self.max_level[last][r] = l
                if r > self.max_run[last][l]:
                    self.max_run[last][l] = r


# ---------------------------------------------------------------- emission --

class Out:
    def __init__(self):
        self.buf = []
        self.report = []

    def w(self, s=""):
        self.buf.append(s)

    def table(self, ctype, name, values, per_line=12, fmt="%d"):
        data = bytes()
        for v in values:
            data += int(v & 0xFFFFFFFF).to_bytes(4, "little")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        self.report.append((name, len(values), crc))
        self.w("/* %s: %d entries, crc32 %08x */" % (name, len(values), crc))
        self.w("static const %s %s[%d] = {" % (ctype, name, len(values)))
        line = "   "
        for i, v in enumerate(values):
            piece = " " + (fmt % v) + ("," if i + 1 < len(values) else "")
            if len(line) + len(piece) > 78:
                self.w(line)
                line = "   "
            line += piece
        if line.strip():
            self.w(line)
        self.w("};")
        self.w()

    def vlc(self, v):
        off, cnt = v.offsets()
        self.table("uint16_t", v.name + "_code", [e[0] for e in v.entries], fmt="0x%04x")
        self.table("uint16_t", v.name + "_sym", [e[2] for e in v.entries])
        self.table("uint16_t", v.name + "_off", off)
        self.table("uint16_t", v.name + "_cnt", cnt)
        self.w("#define %s_MINLEN %d" % (v.name.upper(), v.minlen))
        self.w("#define %s_MAXLEN %d" % (v.name.upper(), v.maxlen))
        self.w("static const m4_vlc %s = { %s_code, %s_sym, %s_off, %s_cnt, %d, %d };"
               % (v.name, v.name, v.name, v.name, v.name, v.minlen, v.maxlen))
        self.w()


def small_vlc(name, code_len_pairs, skip=()):
    entries = []
    for sym, (code, ln) in enumerate(code_len_pairs):
        if sym in skip or ln == 0:
            continue
        entries.append((code, ln, sym))
    return Vlc(name, entries)


HEADER = """\
/* c/lib/video/mpeg4_tables.h -- GENERATED by tools/genmpeg4_tables.py.
 *
 * DO NOT EDIT.  Regenerate with
 *     python3 tools/genmpeg4_tables.py --src <ffmpeg-src> -o c/lib/video/mpeg4_tables.h
 * and check it with
 *     python3 tools/genmpeg4_tables.py --src <ffmpeg-src> --check
 * which must reproduce this file byte for byte.
 *
 * Source: FFmpeg n7.1, libavcodec/{h263data.c,mpeg4data.h,mathtables.c,
 * mpegvideodata.c} -- the machine-readable copy of ISO/IEC 14496-2 Annex B and
 * ITU-T H.263 Tables 12-16.  Every prefix code below was checked for ambiguity
 * by the generator; every array's shape and CRC-32 are in the comment above it.
 */
#ifndef LOGIT_MPEG4_TABLES_H
#define LOGIT_MPEG4_TABLES_H

#include <stdint.h>

/* A prefix code, decoded one bit at a time with a binary search per length.
 * `off[l]`/`cnt[l]` bracket the entries whose codeword is `l` bits long, and
 * within that range `code[]` is ascending, so the search is exact. */
typedef struct {
    const uint16_t *code;
    const uint16_t *sym;
    const uint16_t *off;
    const uint16_t *cnt;
    int minlen, maxlen;
} m4_vlc;

/* A run-level VLC plus the escape-1/escape-2 helper tables (14496-2 7.4.1.3).
 * `esc` is the symbol number of the ESCAPE codeword; symbols below `last_index`
 * carry last=0 and the rest last=1. */
typedef struct {
    const m4_vlc *vlc;
    const uint8_t *run;
    const uint8_t *level;
    const uint8_t *max_level;   /* [2][65], flattened */
    const uint8_t *max_run;     /* [2][65], flattened */
    int n;                      /* symbol number of ESCAPE == n */
    int last_index;
} m4_rl;
"""

FOOTER = """
#endif /* LOGIT_MPEG4_TABLES_H */
"""


def generate(src):
    h263 = strip_comments(read(os.path.join(src, "libavcodec", "h263data.c")))
    h263dsp = strip_comments(read(os.path.join(src, "libavcodec", "h263dsp.c")))
    mp4 = strip_comments(read(os.path.join(src, "libavcodec", "mpeg4data.h")))
    math = strip_comments(read(os.path.join(src, "libavcodec", "mathtables.c")))
    mvd = strip_comments(read(os.path.join(src, "libavcodec", "mpegvideodata.c")))

    o = Out()
    o.w(HEADER)

    # --- H.263 / MPEG-4 shared macroblock-layer codes -----------------------
    icode = find_array(h263, "ff_h263_intra_MCBPC_code", 9)
    ibits = find_array(h263, "ff_h263_intra_MCBPC_bits", 9)
    o.vlc(small_vlc("m4_vlc_intra_mcbpc", list(zip(icode, ibits))))

    pcode = find_array(h263, "ff_h263_inter_MCBPC_code", 28)
    pbits = find_array(h263, "ff_h263_inter_MCBPC_bits", 28)
    # symbols 21..23 are the holes FFmpeg leaves around the stuffing code; they
    # have bits == 0 and are dropped by small_vlc rather than given a codeword.
    o.vlc(small_vlc("m4_vlc_inter_mcbpc", list(zip(pcode, pbits))))

    o.vlc(small_vlc("m4_vlc_cbpy", pairs(find_array(h263, "ff_h263_cbpy_tab", 32))))
    o.vlc(small_vlc("m4_vlc_mvd", pairs(find_array(h263, "ff_mvtab", 66))))

    o.vlc(small_vlc("m4_vlc_dc_lum",
                    pairs(find_array(mp4, "ff_mpeg4_DCtab_lum", 26))))
    o.vlc(small_vlc("m4_vlc_dc_chrom",
                    pairs(find_array(mp4, "ff_mpeg4_DCtab_chrom", 26))))
    o.vlc(small_vlc("m4_vlc_mb_type_b",
                    pairs(find_array(mp4, "ff_mb_type_b_tab", 8))))

    # --- run-level tables ---------------------------------------------------
    rls = [
        RlTable("m4_rl_intra_aic",
                pairs(find_array(h263, "intra_vlc_aic", 206)),
                find_array(h263, "intra_run_aic", 102),
                find_array(h263, "intra_level_aic", 102), 58),
        RlTable("m4_rl_h263_inter",
                pairs(find_array(h263, "ff_inter_vlc", 206)),
                find_array(h263, "ff_inter_run", 102),
                find_array(h263, "ff_inter_level", 102), 58),
        RlTable("m4_rl_mpeg4_intra",
                pairs(find_array(mp4, "ff_mpeg4_intra_vlc", 206)),
                find_array(mp4, "ff_mpeg4_intra_run", 102),
                find_array(mp4, "ff_mpeg4_intra_level", 102), 67),
        RlTable("m4_rl_rvlc_inter",
                pairs(find_array(mp4, "inter_rvlc", 340)),
                find_array(mp4, "inter_rvlc_run", 169),
                find_array(mp4, "inter_rvlc_level", 169), 103),
        RlTable("m4_rl_rvlc_intra",
                pairs(find_array(mp4, "intra_rvlc", 340)),
                find_array(mp4, "intra_rvlc_run", 169),
                find_array(mp4, "intra_rvlc_level", 169), 103),
    ]
    for rl in rls:
        o.vlc(rl.vlc)
        o.table("uint8_t", rl.name + "_run", rl.runs)
        o.table("uint8_t", rl.name + "_level", rl.levels)
        o.table("uint8_t", rl.name + "_maxlevel",
                rl.max_level[0] + rl.max_level[1])
        o.table("uint8_t", rl.name + "_maxrun", rl.max_run[0] + rl.max_run[1])
        o.w("static const m4_rl %s = { &%s_vlc, %s_run, %s_level, "
            "%s_maxlevel, %s_maxrun, %d, %d };"
            % (rl.name, rl.name, rl.name, rl.name, rl.name, rl.name,
               rl.n, rl.last_index))
        o.w()

    # --- scalar tables ------------------------------------------------------
    o.table("uint8_t", "m4_zigzag", find_array(math, "ff_zigzag_direct", 64))
    o.table("uint8_t", "m4_scan_alt_h",
            find_array(mvd, "ff_alternate_horizontal_scan", 64))
    o.table("uint8_t", "m4_scan_alt_v",
            find_array(mvd, "ff_alternate_vertical_scan", 64))
    o.table("uint8_t", "m4_y_dc_scale",
            find_array(mp4, "ff_mpeg4_y_dc_scale_table", 32))
    o.table("uint8_t", "m4_c_dc_scale",
            find_array(mp4, "ff_mpeg4_c_dc_scale_table", 32))
    o.table("uint8_t", "m4_dc_threshold",
            find_array(mp4, "ff_mpeg4_dc_threshold", 8))
    o.table("uint8_t", "m4_identity_qscale",
            find_array(mvd, "ff_default_chroma_qscale_table", 32))
    o.table("uint8_t", "m4_h263_chroma_qscale",
            find_array(h263, "ff_h263_chroma_qscale_table", 32))
    o.table("uint8_t", "m4_modified_quant",
            find_array(h263, "ff_modified_quant_tab", 64))
    o.table("uint16_t", "m4_mba_max", find_array(h263, "ff_mba_max", 6))
    o.table("uint8_t", "m4_mba_length", find_array(h263, "ff_mba_length", 7))
    o.table("uint8_t", "m4_aic_dc_scale",
            find_array(h263, "ff_aic_dc_scale_table", 32))
    # NOTE: there is deliberately no m4_mpeg1_dc_scale table.  FFmpeg reaches
    # for ff_mpeg1_dc_scale_table on the short-video-header path, but that
    # array is ff_mpeg12_dc_scale_table[0], i.e. the constant 8 repeated 32
    # times -- H.263 quantises the intra DC with a fixed step of 8 (H.263
    # 5.4.1).  Emitting 32 identical bytes would look like data and is a
    # constant; mpeg4_mb.c writes the 8.
    o.table("uint16_t", "m4_default_intra_matrix",
            find_array(mp4, "ff_mpeg4_default_intra_matrix", 64))
    o.table("uint16_t", "m4_default_inter_matrix",
            find_array(mp4, "ff_mpeg4_default_non_intra_matrix", 64))
    # ff_h263_format is declared [8][2] and only six rows are written; C zeroes
    # the rest and so do we, explicitly, rather than letting the shape check
    # pass on a short initialiser.  Rows 6 and 7 are "reserved" and
    # "extended PTYPE", both of which mpeg4_hdr.c refuses by name.
    o.table("uint16_t", "m4_h263_format",
            find_array(h263, "ff_h263_format", 12) + [0, 0, 0, 0])
    # ff_h263_round_chroma()'s table (libavcodec/motion_est.h): the special
    # rounding a 4MV macroblock's four luma vectors are folded through to make
    # one chroma vector.
    # H.263 Annex J Table 12: the loop filter's strength per QUANT. Small
    # enough to type and therefore exactly the size of table that gets typed
    # wrong; it comes out of the reference source like everything else here.
    o.table("uint8_t", "m4_h263_loop_strength",
            find_array(h263dsp, "ff_h263_loop_filter_strength", 32))
    o.table("uint8_t", "m4_chroma_roundtab",
            [0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1])

    o.w(FOOTER)
    return "\n".join(o.buf) + "\n", o.report, rls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="FFmpeg source directory")
    ap.add_argument("-o", "--out", default="c/lib/video/mpeg4_tables.h")
    ap.add_argument("--check", action="store_true",
                    help="regenerate and diff against --out instead of writing")
    args = ap.parse_args()

    text, report, rls = generate(args.src)

    for name, n, crc in report:
        print("  %-34s %5d  crc32 %08x" % (name, n, crc))
    for rl in rls:
        print("  %-34s kraft %.6f  esc %x/%d  last_index %d"
              % (rl.name + " (prefix code)", rl.vlc.kraft(),
                 rl.escape[0], rl.escape[1], rl.last_index))

    if args.check:
        if not os.path.exists(args.out):
            print("genmpeg4_tables: %s does not exist" % args.out)
            return 1
        have = read(args.out)
        if have.replace("\r\n", "\n") != text:
            print("genmpeg4_tables: MISMATCH -- %s is not what the generator "
                  "produces from %s" % (args.out, args.src))
            return 1
        print("genmpeg4_tables: %s reproduced byte for byte" % args.out)
        return 0

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("genmpeg4_tables: wrote %s (%d bytes)" % (args.out, len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
