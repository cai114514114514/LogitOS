#!/usr/bin/env python3
"""Write MPEG-2 elementary streams that FFmpeg's ENCODER cannot produce.

WHY THIS EXISTS. tools/genmpeg12.sh builds its corpus by encoding with
`ffmpeg -c:v mpeg2video` and decoding the result with ffmpeg as the oracle.
That covers progressive frame pictures and, with `-flags +ildct+ilme`,
interlaced FRAME pictures with field DCT and field prediction. It cannot cover
three features of broadcast MPEG-2, and the reason is in FFmpeg's own encoder:

    libavcodec/mpeg12enc.c:401   av_assert0(s->c.picture_structure == PICT_FRAME);

so no FIELD picture, therefore no 16x8 motion compensation (which exists only
in field pictures), and `grep -i dual mpeg12enc.c` returns nothing, so no dual
prime either. Encoding harder content does not help: those three syntax
elements are never written.

Rather than leave a third of 13818-2's motion compensation ungated -- which
would mean shipping code no test has ever run -- this writes the bitstream
directly. The streams it produces are ordinary conforming MPEG-2 that FFmpeg
DECODES, so the oracle is unchanged: ffmpeg -idct simple decodes these exactly
as it decodes the encoded corpus, and our decoder must match it byte for byte.

Every VLC table used here is read from the same reference sources
tools/gen_mpeg12_tables.py reads, through its own extraction functions, so the
encoder and the decoder cannot drift apart by one of them being hand-typed.

Usage:  genmpeg12_interlaced.py <outdir>
Writes: field-16x8.m2v  field-dmv.m2v  frame-dmv.m2v  field-intra.m2v
"""
import os
import sys
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "gen_mpeg12_tables", os.path.join(HERE, "gen_mpeg12_tables.py"))
G = importlib.util.module_from_spec(spec)
spec.loader.exec_module(G)

W, H = 128, 96                      # 8 x 6 macroblocks, 8 x 3 per field
MBW = W // 16
MBH_FRAME = ((H + 31) // 32) * 2    # non-progressive: whole macroblocks per FIELD
MBH_FIELD = MBH_FRAME // 2


# ---------------------------------------------------------------- tables --
def load_tables():
    src = {}
    for f in G.FILES:
        with open(G.fetch(f), "r", encoding="utf-8", errors="replace") as fh:
            src[f] = fh.read()
    d, v = src["mpeg12data.c"], src["mpeg12.c"]
    t = {}
    t["dc_lum"] = list(zip(G.ints(d, "ff_mpeg12_vlc_dc_lum_code", 12),
                           G.ints(d, "ff_mpeg12_vlc_dc_lum_bits", 12)))
    t["dc_chroma"] = list(zip(G.ints(d, "ff_mpeg12_vlc_dc_chroma_code", 12),
                              G.ints(d, "ff_mpeg12_vlc_dc_chroma_bits", 12)))
    t["mbincr"] = G.pairs(d, "ff_mpeg12_mbAddrIncrTable", 36)
    t["cbp"] = G.pairs(d, "ff_mpeg12_mbPatTable", 64)
    t["mv"] = G.pairs(d, "ff_mpeg12_mbMotionVectorTable", 17)
    coef = G.pairs(d, "ff_mpeg1_vlc_table", 113)
    t["escape"] = coef[111]
    t["eob"] = coef[112]
    t["ptype"] = {f: (c, b) for c, b, f in G.flagged_pairs(v, "table_mb_ptype", 7)}
    return t


T = load_tables()

MB_INTRA, MB_PAT, MB_BACK, MB_FOR, MB_QUANT = 1, 2, 4, 8, 16


# ----------------------------------------------------------- bit writer --
class BW:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, bits, val):
        assert bits >= 0
        if bits == 0:
            return
        assert 0 <= val < (1 << bits), "value %d does not fit %d bits" % (val, bits)
        self.acc = (self.acc << bits) | val
        self.n += bits
        while self.n >= 8:
            self.n -= 8
            self.buf.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1

    def vlc(self, cb):
        self.put(cb[1], cb[0])

    def signed(self, bits, val):
        self.put(bits, val & ((1 << bits) - 1))

    def align(self):
        if self.n:
            self.put(8 - self.n, 0)

    def start_code(self, code):
        self.align()
        self.buf += bytes([0, 0, 1, code])


# ------------------------------------------------------------- elements --
def dc_diff(bw, diff, chroma):
    """dct_dc_size + dct_dc_differential (7.2.1)."""
    tab = T["dc_chroma"] if chroma else T["dc_lum"]
    if diff == 0:
        bw.vlc(tab[0])
        return
    a = abs(diff)
    size = a.bit_length()
    assert size <= 11
    bw.vlc(tab[size])
    bw.put(size, diff if diff > 0 else diff + (1 << size) - 1)


def coef_escape(bw, run, level):
    """Escape: run(6) then a 12-bit signed level. Any (run, level) at all,
    which is why the generated blocks use nothing else -- the point of these
    streams is the motion syntax, not the coefficient table (the encoded
    corpus hammers that)."""
    assert 0 <= run <= 63 and -2047 <= level <= 2047 and level != 0
    bw.vlc(T["escape"])
    bw.put(6, run)
    bw.signed(12, level)


def eob(bw):
    bw.vlc(T["eob"])


class RNG:
    """A fixed LCG: the corpus must be the same on every machine and on every
    run, or the committed expectations mean nothing."""
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next(self, n):
        self.s = (self.s * 1103515245 + 12345) & 0x7FFFFFFF
        return (self.s >> 7) % n


# --------------------------------------------------------------- headers --
def sequence(bw):
    bw.start_code(0xB3)
    bw.put(12, W)
    bw.put(12, H)
    bw.put(4, 1)          # aspect_ratio_information: square
    bw.put(4, 3)          # frame_rate_code: 25
    bw.put(18, 20000)     # bit_rate_value
    bw.put(1, 1)          # marker_bit
    bw.put(10, 112)       # vbv_buffer_size_value
    bw.put(1, 0)          # constrained_parameters_flag
    bw.put(1, 0)          # load_intra_quantiser_matrix
    bw.put(1, 0)          # load_non_intra_quantiser_matrix
    bw.align()

    bw.start_code(0xB5)
    bw.put(4, 1)          # sequence extension
    bw.put(8, 0x48)       # profile_and_level_indication: Main@Main
    bw.put(1, 0)          # progressive_sequence -- the whole point
    bw.put(2, 1)          # chroma_format 4:2:0
    bw.put(2, 0)          # horizontal_size_extension
    bw.put(2, 0)          # vertical_size_extension
    bw.put(12, 0)         # bit_rate_extension
    bw.put(1, 1)          # marker_bit
    bw.put(8, 0)          # vbv_buffer_size_extension
    bw.put(1, 0)          # low_delay
    bw.put(2, 0)          # frame_rate_extension_n
    bw.put(5, 0)          # frame_rate_extension_d
    bw.align()


def gop(bw):
    bw.start_code(0xB8)
    bw.put(25, 0)         # time_code
    bw.put(1, 1)          # closed_gop
    bw.put(1, 0)          # broken_link
    bw.align()


def picture(bw, temporal_ref, ptype, structure, fcode, tff, fpfd):
    bw.start_code(0x00)
    bw.put(10, temporal_ref)
    bw.put(3, ptype)
    bw.put(16, 0xFFFF)    # vbv_delay
    if ptype in (2, 3):
        bw.put(1, 0)      # full_pel_forward_vector
        bw.put(3, 7)      # forward_f_code, reserved in MPEG-2
    if ptype == 3:
        bw.put(1, 0)
        bw.put(3, 7)
    bw.put(1, 0)          # extra_bit_picture
    bw.align()

    bw.start_code(0xB5)
    bw.put(4, 8)          # picture coding extension
    bw.put(4, fcode)      # f_code[0][0]
    bw.put(4, fcode)      # f_code[0][1]
    bw.put(4, 15)         # f_code[1][0]
    bw.put(4, 15)         # f_code[1][1]
    bw.put(2, 0)          # intra_dc_precision: 8 bits
    bw.put(2, structure)
    bw.put(1, tff)        # top_field_first (0 in a field picture, 6.3.10)
    bw.put(1, fpfd)       # frame_pred_frame_dct
    bw.put(1, 0)          # concealment_motion_vectors
    bw.put(1, 0)          # q_scale_type
    bw.put(1, 0)          # intra_vlc_format
    bw.put(1, 0)          # alternate_scan
    bw.put(1, 0)          # repeat_first_field
    bw.put(1, 0)          # chroma_420_type
    bw.put(1, 0)          # progressive_frame
    bw.put(1, 0)          # composite_display_flag
    bw.align()


# ------------------------------------------------------------- pictures --
def intra_blocks(bw, rng, dcpred):
    """Six intra blocks: a DC step plus a few escape-coded AC coefficients, so
    the reconstruction has real texture for the P pictures to move around.
    Flat blocks would make half-pel interpolation a no-op everywhere except a
    block edge, and half of the motion path would go unmeasured."""
    for n in range(6):
        comp = 0 if n < 4 else (n & 1) + 1
        step = rng.next(41) - 20
        newdc = max(24, min(232, dcpred[comp] + step))
        dc_diff(bw, newdc - dcpred[comp], comp != 0)
        dcpred[comp] = newdc
        pos = 0
        for _ in range(rng.next(4) + 1):
            run = rng.next(6)
            if pos + run + 1 > 63:
                break
            pos += run + 1
            lv = rng.next(9) - 4
            coef_escape(bw, run, lv if lv else 5)
        eob(bw)


def inter_blocks(bw, rng, cbp):
    for n in range(6):
        if not (cbp & (1 << (5 - n))):
            continue
        pos = 0
        for _ in range(rng.next(3) + 1):
            run = rng.next(5)
            if pos + run + 1 > 63:
                break
            pos += run + 1
            lv = rng.next(7) - 3
            coef_escape(bw, run, lv if lv else 2)
        eob(bw)


def mv_delta(bw, want, pred, fcode):
    """Encode one motion vector component: the difference from the predictor,
    wrapped into the f_code's range exactly as 7.6.3.1 unwraps it."""
    rsize = fcode - 1
    f = 1 << rsize
    low, high, rng_ = -16 * f, 16 * f - 1, 32 * f
    delta = want - pred
    while delta > high:
        delta -= rng_
    while delta < low:
        delta += rng_
    if delta == 0:
        bw.vlc(T["mv"][0])
        return want
    if rsize == 0:
        code, resid = abs(delta), 0
    else:
        a = abs(delta)
        code = ((a - 1) >> rsize) + 1
        resid = (a - 1) & (f - 1)
    assert 1 <= code <= 16, "motion_code %d out of Table B-10" % code
    bw.vlc(T["mv"][code])
    bw.put(1, 1 if delta < 0 else 0)
    if rsize:
        bw.put(rsize, resid)
    return want


PH_FIELD = MBH_FIELD * 16          # lines in one field of the coded picture


def pick_mv(rng, span, lo, hi):
    """A vector in [-span, span] half-samples, forced inside the reference.

    13818-2 requires the referenced region to lie within the coded picture, so
    a generator that picks freely produces an ILLEGAL stream -- and the two
    decoders then disagree about something the standard does not define
    (FFmpeg declines the prediction outright, we clamp and count). That
    disagreement is not a finding, it is a bad corpus, which is why the range
    is computed per macroblock rather than fixed."""
    if lo > hi:
        return 0
    v = rng.next(2 * span + 1) - span
    return max(lo, min(hi, v))


def xr(mbx, margin=0):
    return -mbx * 32 + margin, (MBW - 1 - mbx) * 32 - 2 - margin


def yr(row_base, h, ph, margin=0):
    return -row_base * 2 + margin, (ph - h - row_base) * 2 - 2 - margin


def slice_header(bw, row, qscale_code):
    bw.start_code(row + 1)
    bw.put(5, qscale_code)
    bw.put(1, 0)          # extra_bit_slice


def field_picture_body(bw, rng, ptype, structure, mode, fcode):
    """One field picture. `mode` selects the motion syntax under test:
    'intra', '16x8', 'field', 'dmv'."""
    rows = MBH_FIELD
    for row in range(rows):
        slice_header(bw, row, 8)
        dc = [128, 128, 128]
        pmv = [[0, 0], [0, 0]]     # last_mv[0][j][k]
        x = 0
        first = True
        while x < MBW:
            # macroblock_address_increment; a skipped run in the middle of the
            # row exercises the skip path, never at the start or the end of a
            # slice (6.2.4 forbids both).
            inc = 1
            if not first and x + 2 < MBW and rng.next(4) == 0:
                inc = 2
            if inc > 1:
                # skipped macroblocks reset the predictors, 7.6.6
                pmv = [[0, 0], [0, 0]]
                dc = [128, 128, 128]
            bw.vlc(T["mbincr"][inc - 1])
            x += inc - 1
            first = False

            if ptype == 1 or mode == "intra":
                bw.vlc(T["ptype"][MB_INTRA] if ptype == 2 else (1, 1))
                intra_blocks(bw, rng, dc)
                pmv = [[0, 0], [0, 0]]
                dc = dc
            else:
                want_pat = rng.next(2) == 0
                flags = MB_FOR | (MB_PAT if want_pat else 0)
                bw.vlc(T["ptype"][flags])
                if mode == "16x8":
                    bw.put(2, 2)          # field_motion_type: 16x8
                    for j in range(2):
                        bw.put(1, rng.next(2))            # field_select
                        lo, hi = xr(x)
                        pmv[j][0] = mv_delta(bw, pick_mv(rng, 6, lo, hi),
                                             pmv[j][0], fcode)
                        lo, hi = yr(row * 16 + j * 8, 8, PH_FIELD)
                        pmv[j][1] = mv_delta(bw, pick_mv(rng, 6, lo, hi),
                                             pmv[j][1], fcode)
                elif mode == "field":
                    bw.put(2, 1)          # field_motion_type: field-based
                    bw.put(1, rng.next(2))
                    lo, hi = xr(x)
                    v = mv_delta(bw, pick_mv(rng, 6, lo, hi), pmv[0][0], fcode)
                    pmv[0][0] = pmv[1][0] = v
                    lo, hi = yr(row * 16, 16, PH_FIELD)
                    v = mv_delta(bw, pick_mv(rng, 6, lo, hi), pmv[0][1], fcode)
                    pmv[0][1] = pmv[1][1] = v
                else:                     # dual prime
                    bw.put(2, 3)
                    lo, hi = xr(x, 6)
                    v = mv_delta(bw, pick_mv(rng, 6, lo, hi), pmv[0][0], fcode)
                    pmv[0][0] = pmv[1][0] = v
                    put_dmvector(bw, rng.next(3) - 1)
                    lo, hi = yr(row * 16, 16, PH_FIELD, 6)
                    v = mv_delta(bw, pick_mv(rng, 6, lo, hi), pmv[0][1], fcode)
                    pmv[0][1] = pmv[1][1] = v
                    put_dmvector(bw, rng.next(3) - 1)
                if want_pat:
                    cbp = rng.next(63) + 1
                    bw.vlc(T["cbp"][cbp])
                    inter_blocks(bw, rng, cbp)
                dc = [128, 128, 128]
            x += 1
        bw.align()


def put_dmvector(bw, v):
    if v == 0:
        bw.put(1, 0)
    else:
        bw.put(1, 1)
        bw.put(1, 0 if v > 0 else 1)


def frame_picture_body(bw, rng, mode, fcode, tff):
    """A P FRAME picture whose macroblocks use field-based or dual-prime
    prediction -- both legal in a frame picture when frame_pred_frame_dct is
    0, and dual prime there is the case with the 1-or-3 field-distance scaling
    that the field-picture form does not have."""
    for row in range(MBH_FRAME):
        slice_header(bw, row, 8)
        pmv = [[0, 0], [0, 0]]
        x = 0
        first = True
        while x < MBW:
            inc = 1
            if not first and x + 2 < MBW and rng.next(5) == 0:
                inc = 2
            if inc > 1:
                pmv = [[0, 0], [0, 0]]
            bw.vlc(T["mbincr"][inc - 1])
            x += inc - 1
            first = False

            want_pat = rng.next(2) == 0
            bw.vlc(T["ptype"][MB_FOR | (MB_PAT if want_pat else 0)])
            if mode == "dmv":
                bw.put(2, 3)
                if want_pat:
                    bw.put(1, rng.next(2))          # dct_type
                lo, hi = xr(x, 6)
                v = mv_delta(bw, pick_mv(rng, 6, lo, hi), pmv[0][0], fcode)
                pmv[0][0] = pmv[1][0] = v
                put_dmvector(bw, rng.next(3) - 1)
                # a field vector inside a frame picture counts FIELD lines
                lo, hi = yr(row * 8, 8, PH_FIELD, 6)
                got = mv_delta(bw, pick_mv(rng, 4, lo, hi), pmv[0][1] >> 1, fcode)
                pmv[0][1] = pmv[1][1] = got * 2
                put_dmvector(bw, rng.next(3) - 1)
            else:                                   # field-based
                bw.put(2, 1)
                if want_pat:
                    bw.put(1, rng.next(2))
                for j in range(2):
                    bw.put(1, rng.next(2))
                    lo, hi = xr(x)
                    pmv[j][0] = mv_delta(bw, pick_mv(rng, 6, lo, hi),
                                         pmv[j][0], fcode)
                    lo, hi = yr(row * 8, 8, PH_FIELD)
                    got = mv_delta(bw, pick_mv(rng, 4, lo, hi),
                                   pmv[j][1] >> 1, fcode)
                    pmv[j][1] = got * 2
            if want_pat:
                cbp = rng.next(63) + 1
                bw.vlc(T["cbp"][cbp])
                inter_blocks(bw, rng, cbp)
            x += 1
        bw.align()


def intra_field_pair(bw, rng, tref):
    """An I frame coded as two I field pictures -- the anchor everything else
    predicts from, and by itself the gate on field-picture INTRA decoding
    (which has its own DC predictor reset and its own slice geometry)."""
    for structure in (1, 2):
        picture(bw, tref, 1, structure, 1, 0, 0)
        for row in range(MBH_FIELD):
            slice_header(bw, row, 8)
            dc = [128, 128, 128]
            for _ in range(MBW):
                bw.vlc(T["mbincr"][0])
                bw.put(1, 1)                  # macroblock_type: intra
                intra_blocks(bw, rng, dc)
            bw.align()


def build(kind):
    bw = BW()
    rng = RNG(0x5EED)
    sequence(bw)
    gop(bw)
    intra_field_pair(bw, rng, 0)

    if kind == "field-intra":
        for t in (1, 2, 3):
            intra_field_pair(bw, rng, t)
    elif kind == "field-16x8":
        for t in (1, 2, 3):
            for structure in (1, 2):
                picture(bw, t, 2, structure, 1, 0, 0)
                field_picture_body(bw, rng, 2, structure,
                                   "16x8" if t % 2 else "field", 1)
    elif kind == "field-dmv":
        for t in (1, 2, 3):
            for structure in (1, 2):
                picture(bw, t, 2, structure, 1, 0, 0)
                field_picture_body(bw, rng, 2, structure, "dmv", 1)
    elif kind == "frame-dmv":
        for t in (1, 2, 3):
            picture(bw, t, 2, 3, 1, 1, 0)
            frame_picture_body(bw, rng, "dmv", 1, 1)
    elif kind == "frame-field":
        for t in (1, 2, 3):
            picture(bw, t, 2, 3, 1, 1, 0)
            frame_picture_body(bw, rng, "field", 1, 1)
    else:
        raise SystemExit("unknown stream kind " + kind)

    bw.start_code(0xB7)          # sequence_end_code
    bw.align()
    return bytes(bw.buf)


KINDS = ["field-intra", "field-16x8", "field-dmv", "frame-dmv", "frame-field"]


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: genmpeg12_interlaced.py <outdir>")
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for k in KINDS:
        data = build(k)
        path = os.path.join(out, k + ".m2v")
        with open(path, "wb") as f:
            f.write(data)
        print("  %-14s %6d bytes" % (k, len(data)))


if __name__ == "__main__":
    main()
