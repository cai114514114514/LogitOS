#!/usr/bin/env python3
"""Emit c/lib/video/mpeg12_tables.{h,c} -- every constant table MPEG-1/2 video
needs, extracted mechanically from reference C source. DO NOT TYPE THESE IN.

WHY A GENERATOR. ISO/IEC 13818-2 Annex B is ~130 variable-length codes for the
DCT coefficients alone, plus the macroblock-address, coded-block-pattern,
motion-code, DC-size and macroblock-type tables. A wrong entry in a VLC table
does not shade a pixel: the bit pointer lands in the middle of the next code
and everything after it in that slice is noise. One wrong bit among a few
thousand is not findable by reading, and a decoder that *mostly* works is the
worst outcome because the corpus still produces pictures. So the arrays are cut
out of a reference implementation's own source and transcribed mechanically,
exactly as tools/gen_vp8_tables.py does with RFC 6386's reference decoder.

PROVENANCE. The reference source is FFmpeg's libavcodec (mpeg12data.c,
mpeg12.c, mpegvideodata.c, mathtables.c) -- the same implementation this
decoder's gate diffs against, which is deliberate: a table disagreement and an
arithmetic disagreement would otherwise be indistinguishable in the gate's
output. Those four files carry nothing but ISO tables; the decoder in
c/lib/video/mpeg12*.c is written from the standard's own pseudo-code and shares
no line with them (see the header of mpeg12.c for the one place -- the IDCT --
where matching FFmpeg bit-for-bit is the stated goal and how it was done).

The sources are fetched once into build/mpeg12ref/src/ and are NOT vendored;
the generated .h/.c ARE committed, so a build without network works:

    python3 tools/gen_mpeg12_tables.py            # fetch if missing, write
    python3 tools/gen_mpeg12_tables.py --check    # regenerate, diff, exit 1

It prints the shape and CRC-32 of every array it emits. --check is what a CI
target runs; it fails on any difference, which is what stops a hand edit to the
generated file from surviving.
"""
import os
import re
import sys
import zlib
import subprocess

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCDIR = os.path.join(HERE, "build", "mpeg12ref", "src")
OUT_H = os.path.join(HERE, "c", "lib", "video", "mpeg12_tables.h")
OUT_C = os.path.join(HERE, "c", "lib", "video", "mpeg12_tables.c")

BASE = "https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/"
FILES = ["mpeg12data.c", "mpeg12.c", "mpegvideodata.c", "mathtables.c"]

VLC_BITS = 9          # first-level index width, all tables


# --------------------------------------------------------------------------
# fetching / parsing
# --------------------------------------------------------------------------
def fetch(name):
    path = os.path.join(SRCDIR, name)
    if os.path.exists(path):
        return path
    os.makedirs(SRCDIR, exist_ok=True)
    url = BASE + name
    sys.stderr.write("fetch %s\n" % url)
    rc = subprocess.call(["curl", "-sfL", "-o", path, url])
    if rc != 0 or not os.path.exists(path):
        sys.stderr.write(
            "cannot fetch %s -- put the reference sources in %s by hand\n"
            % (url, SRCDIR))
        sys.exit(2)
    return path


def strip_comments(text, keep=False):
    """Remove /* */ and // comments. keep=True leaves // comments alone --
    table_mb_ptype's flag values live in them and are extracted, not guessed."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    if not keep:
        text = re.sub(r"//[^\n]*", " ", text)
    return text


def array_body(text, name):
    """The balanced-brace initialiser of `... name[...] = { ... };`."""
    m = re.search(r"\b" + re.escape(name) + r"\s*(?:\[[^\]]*\]\s*)*=\s*\{", text)
    if not m:
        raise KeyError(name)
    i = m.end() - 1
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
    raise ValueError("unbalanced initialiser for " + name)


NUM = re.compile(r"-?\b(?:0[xX][0-9a-fA-F]+|\d+)\b")


def ints(text, name, count=None):
    body = strip_comments(array_body(strip_comments(text), name))
    vals = [int(v, 0) for v in NUM.findall(body)]
    if count is not None:
        if len(vals) < count:
            raise ValueError("%s: got %d ints, want %d" % (name, len(vals), count))
        vals = vals[:count]
    return vals


def pairs(text, name, count):
    v = ints(text, name, count * 2)
    return [(v[2 * i], v[2 * i + 1]) for i in range(count)]


def flagged_pairs(text, name, count):
    """{ code, bits }, // 0xNN NAME|NAME  -- returns (code, bits, flagvalue).

    The flag value is READ FROM THE SOURCE, not inferred from the names: a
    macroblock_type table whose codes are right and whose meanings are guessed
    decodes every stream into a plausible wrong picture."""
    body = array_body(strip_comments(text, keep=True), name)
    out = []
    for line in body.split("\n"):
        line = line.strip()
        if not line or not line.startswith("{"):
            continue
        m = re.match(r"\{\s*(\w+)\s*,\s*(\w+)\s*\}\s*,?\s*//\s*(0[xX][0-9a-fA-F]+)", line)
        if not m:
            raise ValueError("%s: cannot read '%s'" % (name, line))
        out.append((int(m.group(1), 0), int(m.group(2), 0), int(m.group(3), 0)))
    if len(out) != count:
        raise ValueError("%s: %d rows, want %d" % (name, len(out), count))
    return out


# --------------------------------------------------------------------------
# two-level VLC construction
# --------------------------------------------------------------------------
def build_vlc(name, codes):
    """codes: list of (code, length, symbol). Returns a flat entry list.

    Entry = (sym, len, sub).  sub==0 and len>0: a leaf, consume `len` bits,
    symbol `sym`.  sub==1: consume VLC_BITS then `len` more and index
    entries[sym + those bits].  len==0: no such code.

    Prefix-freeness is not assumed, it is CHECKED -- every collision is a
    fatal error here rather than a decoder that reads the wrong symbol.
    """
    for c, l, s in codes:
        if l < 1 or l > 16:
            raise ValueError("%s: length %d out of range" % (name, l))
        if c >> l:
            raise ValueError("%s: code 0x%x does not fit %d bits" % (name, c, l))

    n1 = 1 << VLC_BITS
    ent = [None] * n1                       # first level
    subs = {}                               # prefix -> {index: (sym, len)}

    for code, length, sym in codes:
        if length <= VLC_BITS:
            base = code << (VLC_BITS - length)
            for k in range(1 << (VLC_BITS - length)):
                if ent[base + k] is not None:
                    raise ValueError("%s: code collision at 0x%x" % (name, base + k))
                ent[base + k] = (sym, length, 0)
        else:
            pre = code >> (length - VLC_BITS)
            extra = length - VLC_BITS
            sub = subs.setdefault(pre, {})
            sub[("bits", 0)] = max(sub.get(("bits", 0), 0), extra)
            rest = code & ((1 << extra) - 1)
            sub[(rest, extra)] = sym

    # widest suffix per subtable, then fill
    entries = list(ent)
    for pre in sorted(subs):
        sub = subs[pre]
        width = sub[("bits", 0)]
        if entries[pre] is not None:
            raise ValueError("%s: prefix 0x%x is both a leaf and a node" % (name, pre))
        base = len(entries)
        entries[pre] = (base, width, 1)
        entries.extend([None] * (1 << width))
        for key, sym in sub.items():
            if key == ("bits", 0):
                continue
            rest, extra = key
            off = rest << (width - extra)
            for k in range(1 << (width - extra)):
                if entries[base + off + k] is not None:
                    raise ValueError("%s: sub collision" % name)
                entries[base + off + k] = (sym, extra, 0)

    entries = [(0, 0, 0) if e is None else e for e in entries]

    kraft = sum(2.0 ** -l for _, l, _ in codes)
    return entries, kraft


def decodes(entries, pattern, width):
    """Run the C decoder's exact algorithm over `width` bits, MSB first.
    Returns the symbol, or None when the pattern names no code."""
    e = entries[(pattern >> (width - VLC_BITS)) & ((1 << VLC_BITS) - 1)]
    if e[2]:
        rest = (pattern >> (width - VLC_BITS - e[1])) & ((1 << e[1]) - 1)
        e = entries[e[0] + rest]
    if e[1] == 0:
        return None
    return e[0]


# --------------------------------------------------------------------------
# emission
# --------------------------------------------------------------------------
class Emit:
    def __init__(self):
        self.h = []
        self.c = []
        self.report = []

    def note(self, label, shape, data):
        blob = b"".join(int(x).to_bytes(4, "little", signed=True) for x in data)
        self.report.append("  %-28s %-14s crc32=%08x" %
                           (label, shape, zlib.crc32(blob) & 0xFFFFFFFF))

    def arr(self, ctype, name, values, per_line=12, flat_note=None):
        self.h.append("extern const %s %s[%d];" % (ctype, name, len(values)))
        rows = []
        for i in range(0, len(values), per_line):
            rows.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
        self.c.append("const %s %s[%d] = {\n%s\n};\n" %
                      (ctype, name, len(values), "\n".join(rows)))
        self.note(name, flat_note or ("[%d]" % len(values)), values)

    def vlc(self, name, entries, kraft):
        self.h.append("extern const m12_vlc_e %s[%d];" % (name, len(entries)))
        rows = []
        for i in range(0, len(entries), 6):
            chunk = entries[i:i + 6]
            rows.append("    " + " ".join("{%d,%d,%d}," % e for e in chunk))
        self.c.append("const m12_vlc_e %s[%d] = {\n%s\n};\n" %
                      (name, len(entries), "\n".join(rows)))
        flat = []
        for s, l, sub in entries:
            flat += [s, l, sub]
        self.note(name, "[%d] kraft=%.6f" % (len(entries), kraft), flat)


def main():
    check = "--check" in sys.argv
    src = {}
    for f in FILES:
        with open(fetch(f), "r", encoding="utf-8", errors="replace") as fh:
            src[f] = fh.read()

    d = src["mpeg12data.c"]
    v = src["mpeg12.c"]
    mv = src["mpegvideodata.c"]
    mt = src["mathtables.c"]

    e = Emit()

    # --- scans and matrices ------------------------------------------------
    zig = ints(mt, "ff_zigzag_direct", 64)
    alt = ints(mv, "ff_alternate_vertical_scan", 64)
    if sorted(zig) != list(range(64)) or sorted(alt) != list(range(64)):
        raise ValueError("a scan table is not a permutation of 0..63")
    e.arr("uint8_t", "m12_scan_zigzag", zig, 8)
    e.arr("uint8_t", "m12_scan_alternate", alt, 8)

    di = ints(d, "ff_mpeg1_default_intra_matrix", 64)
    dn = ints(d, "ff_mpeg1_default_non_intra_matrix", 64)
    if di[0] != 8 or any(x < 1 or x > 255 for x in di + dn):
        raise ValueError("default quantiser matrix out of range")
    e.arr("uint8_t", "m12_default_intra_matrix", di, 8)
    e.arr("uint8_t", "m12_default_non_intra_matrix", dn, 8)

    nlq = ints(mv, "ff_mpeg2_non_linear_qscale", 32)
    if nlq[1] != 1 or nlq[31] != 112:
        raise ValueError("non-linear qscale table does not look like Table 7-6")
    e.arr("uint8_t", "m12_non_linear_qscale", nlq, 8)

    # --- DC size (Tables B-12 / B-13) --------------------------------------
    for who, cn, bn in (("lum", "ff_mpeg12_vlc_dc_lum_code", "ff_mpeg12_vlc_dc_lum_bits"),
                        ("chroma", "ff_mpeg12_vlc_dc_chroma_code",
                         "ff_mpeg12_vlc_dc_chroma_bits")):
        code = ints(d, cn, 12)
        bits = ints(d, bn, 12)
        ent, k = build_vlc(who, [(code[i], bits[i], i) for i in range(12)])
        if abs(k - 1.0) > 1e-12:
            raise ValueError("dc_%s is not a complete prefix code (kraft %g)" % (who, k))
        e.vlc("m12_vlc_dc_" + who, ent, k)

    # --- macroblock_address_increment (Table B-1) --------------------------
    # 36 rows: 0..32 are increments 1..33, then escape, stuffing, and the
    # 8-bit all-zero row FFmpeg carries so its VLC reader can see the start
    # code. That last one is dropped: this decoder detects the end of a slice
    # by peeking 23 zero bits, so a "code" that IS the start-code prefix would
    # swallow it.
    inc = pairs(d, "ff_mpeg12_mbAddrIncrTable", 36)
    codes = [(inc[i][0], inc[i][1], i + 1) for i in range(33)]
    codes.append((inc[33][0], inc[33][1], 34))    # escape -> +33
    codes.append((inc[34][0], inc[34][1], 35))    # stuffing -> ignore
    ent, k = build_vlc("mbincr", codes)
    e.vlc("m12_vlc_mbincr", ent, k)

    # --- coded_block_pattern (Table B-9) -----------------------------------
    # index == cbp; entry 0 (cbp 0) is legal only for 4:2:2/4:4:4, which this
    # decoder refuses by name, and is emitted so the table stays the table.
    pat = pairs(d, "ff_mpeg12_mbPatTable", 64)
    ent, k = build_vlc("mbpat", [(pat[i][0], pat[i][1], i) for i in range(64)])
    e.vlc("m12_vlc_cbp", ent, k)

    # --- motion_code (Table B-10) ------------------------------------------
    mvt = pairs(d, "ff_mpeg12_mbMotionVectorTable", 17)
    ent, k = build_vlc("mv", [(mvt[i][0], mvt[i][1], i) for i in range(17)])
    e.vlc("m12_vlc_motion", ent, k)

    # --- macroblock_type, P and B (Tables B-3 / B-4) -----------------------
    # symbol = the spec's flag set: 1 intra, 2 pattern, 4 backward, 8 forward,
    # 16 quant -- read out of the reference's own comments.
    for who, nm, cnt in (("ptype", "table_mb_ptype", 7), ("btype", "table_mb_btype", 11)):
        rows = flagged_pairs(v, nm, cnt)
        for _, _, fl in rows:
            if fl & ~0x1F:
                raise ValueError("%s: flag 0x%x outside 0x1F" % (nm, fl))
        ent, k = build_vlc(who, [(c, b, f) for c, b, f in rows])
        e.vlc("m12_vlc_mb_" + who, ent, k)

    # --- DCT coefficients (Tables B-14 / B-15) -----------------------------
    run = ints(d, "ff_mpeg12_run", 111)
    lev = ints(d, "ff_mpeg12_level", 111)
    if max(run) != 31 or max(lev) != 40:
        raise ValueError("run/level table does not look like Annex B")
    e.arr("uint8_t", "m12_rl_run", run, 16)
    e.arr("uint8_t", "m12_rl_level", lev, 16)

    # Neither B-14 nor B-15 is a COMPLETE prefix code and the holes are not the
    # same size, so "kraft == 1" is not the check. What IS checked is WHICH
    # 16-bit patterns decode to nothing, by running the C decoder's own
    # algorithm over all 65,536 of them:
    #   - the sixteen patterns of twelve leading zeros must be among them (that
    #     is the head of a start code; no coefficient may be spelled with it),
    #   - every hole must sit behind at least seven leading zeros, i.e. deep in
    #     the code space where the standard leaves room, never where a short
    #     code lives,
    #   - and the number of them must be exactly what the Kraft sum predicts,
    #     which is what catches a table with one entry dropped and another
    #     duplicated -- a shape that leaves the sum unchanged.
    for who, nm in (("b14", "ff_mpeg1_vlc_table"), ("b15", "ff_mpeg2_vlc_table")):
        t = pairs(d, nm, 113)
        codes = [(t[i][0], t[i][1], i) for i in range(113)]
        ent, k = build_vlc(who, codes)
        undec = [p for p in range(1 << 16) if decodes(ent, p, 16) is None]
        predicted = int(round((1.0 - k) * 65536))
        if len(undec) != predicted:
            raise ValueError("%s: %d undecodable patterns, kraft predicts %d"
                             % (nm, len(undec), predicted))
        if any(p >> 4 != 0 for p in undec[:16]) or len(undec) < 16:
            raise ValueError("%s: the twelve-zero prefix is not reserved" % nm)
        lz = min(16 - p.bit_length() for p in undec)
        if lz < 7:
            raise ValueError("%s: a hole sits behind only %d leading zeros" % (nm, lz))
        e.vlc("m12_vlc_coef_" + who, ent, k)
        e.report.append("  %-28s %d/65536 patterns undecodable, deepest hole "
                        "behind %d leading zeros" % ("(" + nm + ")", len(undec), lz))

    # --------------------------------------------------------------------
    head = ("/* GENERATED by tools/gen_mpeg12_tables.py -- DO NOT EDIT.\n"
            " * Source: FFmpeg libavcodec %s (ISO/IEC 13818-2 Annex B tables).\n"
            " * Regenerate with `python3 tools/gen_mpeg12_tables.py`;\n"
            " * `--check` fails if this file and the generator disagree.\n */\n"
            % ", ".join(FILES))

    h = [head,
         "#ifndef LOGIT_MPEG12_TABLES_H",
         "#define LOGIT_MPEG12_TABLES_H",
         "",
         "#include <stdint.h>",
         "",
         "/* Two-level VLC entry. len == 0: no such code (bitstream is corrupt).",
         " * sub == 0: leaf -- consume `len` bits, the symbol is `sym`.",
         " * sub == 1: node -- consume M12_VLC_BITS, then `len` more bits, and",
         " *           look at entry [sym + those bits] of the same array. */",
         "typedef struct { int16_t sym; uint8_t len; uint8_t sub; } m12_vlc_e;",
         "",
         "#define M12_VLC_BITS %d" % VLC_BITS,
         "",
         "/* symbols of m12_vlc_coef_*: 0..110 index m12_rl_run/m12_rl_level */",
         "#define M12_COEF_ESCAPE 111",
         "#define M12_COEF_EOB    112",
         "",
         "/* symbols of m12_vlc_mbincr: 1..33 = increment, 34 = escape (+33),",
         " * 35 = macroblock_stuffing (no increment) */",
         "#define M12_MBINCR_ESCAPE  34",
         "#define M12_MBINCR_STUFF   35",
         "",
         "/* macroblock_type flags, the symbols of m12_vlc_mb_ptype/btype */",
         "#define M12_MB_INTRA   0x01",
         "#define M12_MB_PATTERN 0x02",
         "#define M12_MB_BACKWARD 0x04",
         "#define M12_MB_FORWARD 0x08",
         "#define M12_MB_QUANT   0x10",
         ""] + e.h + ["", "#endif /* LOGIT_MPEG12_TABLES_H */", ""]

    c = [head, '#include "mpeg12_tables.h"', ""] + e.c

    out_h = "\n".join(h)
    out_c = "\n".join(c)

    print("mpeg12 tables:")
    for line in e.report:
        print(line)
    print("  %d arrays, %d bytes of .h, %d bytes of .c" %
          (len(e.report), len(out_h), len(out_c)))

    if check:
        bad = 0
        for path, want in ((OUT_H, out_h), (OUT_C, out_c)):
            try:
                with open(path, "r", encoding="utf-8", newline="") as fh:
                    have = fh.read()
            except OSError:
                have = None
            if have != want:
                print("MISMATCH: %s differs from the generator's output" % path)
                bad = 1
        if bad:
            print("MPEG12-TABLES-CHECK-FAIL")
            return 1
        print("MPEG12-TABLES-CHECK-OK: generated files match the generator")
        return 0

    for path, data in ((OUT_H, out_h), (OUT_C, out_c)):
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(data)
        print("  wrote %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
