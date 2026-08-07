#include "cff.h"
#include "fontrd.h"

/* CFF / CFF2 reader and Type 2 charstring interpreter. See cff.h for scope.
 *
 * Numbers inside the interpreter are 16.16 fixed. Charstrings may carry
 * fractional coordinates (the 255 operator is itself 16.16), and rounding each
 * delta as it arrives would let error accumulate along a contour, so the current
 * point is kept at full precision and rounded once per emitted point. */

#define FIX_ONE   65536
#define CFF_STACK 513            /* CFF2 raises the Type 2 limit of 48 to 513 */
#define SUBR_DEPTH 10            /* the spec's nesting limit */
#define TRANS_N   32             /* put/get transient array */

static int32_t fixround(int32_t v) { return (int32_t)(((int64_t)v + 32768) >> 16); }

/* --------------------------------------------------------------- INDEX -- */

struct cff_index {
    uint32_t start;              /* absolute offset of the INDEX itself */
    uint32_t count;
    uint32_t offsize;
    uint32_t offarr;             /* absolute offset of the offset array */
    uint32_t data;               /* data base; object offsets are 1-based from here */
    uint32_t end;                /* absolute offset just past the INDEX */
    int ok;
};

static uint32_t rd_off(const struct fr *b, uint32_t p, uint32_t sz)
{
    switch (sz) {
    case 1: return fr_u8(b, p);
    case 2: return fr_u16(b, p);
    case 3: return fr_u24(b, p);
    case 4: return fr_u32(b, p);
    }
    return 0;
}

/* Read an INDEX header at `off`. cff2 selects the 32-bit count. */
static int idx_read(const struct fr *b, uint32_t off, int cff2, struct cff_index *ix)
{
    ix->ok = 0; ix->count = 0; ix->start = off; ix->end = off;
    ix->offsize = 0; ix->offarr = 0; ix->data = 0;
    uint32_t hdr = cff2 ? 4u : 2u;
    if (!fr_ok(b, off, hdr)) return -1;
    ix->count = cff2 ? fr_u32(b, off) : fr_u16(b, off);
    if (ix->count == 0) { ix->end = off + hdr; ix->ok = 1; return 0; }
    if (!fr_ok(b, off + hdr, 1)) return -1;
    ix->offsize = fr_u8(b, off + hdr);
    if (ix->offsize < 1 || ix->offsize > 4) return -1;
    ix->offarr = off + hdr + 1;
    /* count+1 offsets must fit. count comes off the wire, so widen before
     * multiplying and compare against the space actually left in the blob. */
    uint64_t need = ((uint64_t)ix->count + 1) * ix->offsize;
    if (need > b->len || (uint64_t)ix->offarr > (uint64_t)b->len - need) return -1;
    ix->data = ix->offarr + (uint32_t)need - 1;
    uint32_t last = rd_off(b, ix->offarr + (uint32_t)((uint64_t)ix->count * ix->offsize),
                           ix->offsize);
    if (last < 1) return -1;
    uint64_t e = (uint64_t)ix->data + last;
    if (e > b->len) return -1;
    ix->end = (uint32_t)e;
    ix->ok = 1;
    return 0;
}

/* Object `i` of the INDEX -> [*off, *off+*len). Returns 0 or -1. */
static int idx_get(const struct fr *b, const struct cff_index *ix, uint32_t i,
                   uint32_t *off, uint32_t *len)
{
    if (!ix->ok || i >= ix->count) return -1;
    uint32_t a = rd_off(b, ix->offarr + (uint32_t)((uint64_t)i * ix->offsize), ix->offsize);
    uint32_t c = rd_off(b, ix->offarr + (uint32_t)((uint64_t)(i + 1) * ix->offsize), ix->offsize);
    if (a < 1 || c < a) return -1;
    uint64_t s = (uint64_t)ix->data + a, e = (uint64_t)ix->data + c;
    if (e > b->len) return -1;
    *off = (uint32_t)s; *len = (uint32_t)(e - s);
    return 0;
}

/* The count-dependent subroutine index bias (Type 2 spec, "Subroutine Operators"). */
static int subr_bias(uint32_t n)
{
    if (n < 1240) return 107;
    if (n < 33900) return 1131;
    return 32768;
}

/* ---------------------------------------------------------------- DICT -- */

/* A DICT operand as an exact decimal: value = num * 10^exp. Integers are
 * (n, 0); reals keep their mantissa so 1/FontMatrix[0] comes out as the exact
 * units-per-em (0.00048828125 -> 2048) rather than a rounded reciprocal. */
struct dop { int64_t num; int exp; };

#define DICT_MAXOPS 48

static int64_t ipow10(int e)
{
    int64_t v = 1;
    while (e-- > 0) v *= 10;
    return v;
}

static int32_t dop_int(const struct dop *d)
{
    if (d->exp >= 0) {
        if (d->exp > 18) return 0;
        return (int32_t)(d->num * ipow10(d->exp));
    }
    if (-d->exp > 18) return 0;
    return (int32_t)(d->num / ipow10(-d->exp));
}

/* Parse the BCD real that starts at *p (just after the 30 marker). */
static void dict_real(const struct fr *b, uint32_t *p, uint32_t end, struct dop *out)
{
    int64_t mant = 0; int frac = 0, seen_dot = 0, neg = 0;
    int in_exp = 0, exp_neg = 0, expv = 0, over = 0;
    while (*p < end) {
        uint32_t byte = fr_u8(b, (*p)++);
        for (int half = 0; half < 2; half++) {
            uint32_t nib = half ? (byte & 0xF) : (byte >> 4);
            if (nib <= 9) {
                if (in_exp) { if (expv < 1000) expv = expv * 10 + (int)nib; }
                else if (mant < 100000000000000000LL) {
                    mant = mant * 10 + (int)nib;
                    if (seen_dot) frac++;
                } else if (!seen_dot) over++;
            }
            else if (nib == 0xA) seen_dot = 1;
            else if (nib == 0xB) in_exp = 1;
            else if (nib == 0xC) { in_exp = 1; exp_neg = 1; }
            else if (nib == 0xE) { if (in_exp) exp_neg = 1; else neg = 1; }
            else if (nib == 0xF) goto done;
            /* 0xD is reserved: skipped */
        }
    }
done:
    out->num = neg ? -mant : mant;
    out->exp = over - frac + (in_exp ? (exp_neg ? -expv : expv) : 0);
}

/* Scan a DICT for operator `op` (escaped operators are 0x0C00 | b1) and copy its
 * operands. Returns the operand count, or -1 if the operator is absent. */
static int dict_get(const struct fr *b, uint32_t off, uint32_t len, uint32_t op,
                    struct dop *out, int cap)
{
    struct dop st[DICT_MAXOPS];
    int sp = 0;
    uint32_t p = off, end = off + len;
    if (!fr_ok(b, off, len)) return -1;
    while (p < end) {
        uint32_t b0 = fr_u8(b, p);
        if (b0 == 12) {
            if (p + 2 > end) return -1;
            uint32_t b1 = fr_u8(b, p + 1); p += 2;
            if ((0x0C00u | b1) == op) goto hit;
            sp = 0; continue;
        }
        if (b0 <= 21) {
            p += 1;
            if (b0 == op) goto hit;
            sp = 0; continue;
        }
        struct dop d = { 0, 0 };
        if (b0 == 28)      { if (p + 3 > end) return -1; d.num = (int16_t)(uint16_t)fr_u16(b, p + 1); p += 3; }
        else if (b0 == 29) { if (p + 5 > end) return -1; d.num = (int32_t)fr_u32(b, p + 1); p += 5; }
        else if (b0 == 30) { p += 1; dict_real(b, &p, end, &d); }
        else if (b0 <= 246) { d.num = (int)b0 - 139; p += 1; }
        else if (b0 <= 250) { if (p + 2 > end) return -1; d.num = ((int)b0 - 247) * 256 + (int)fr_u8(b, p + 1) + 108; p += 2; }
        else if (b0 <= 254) { if (p + 2 > end) return -1; d.num = -((int)b0 - 251) * 256 - (int)fr_u8(b, p + 1) - 108; p += 2; }
        else return -1;                      /* 22..27, 31, 255: reserved in a DICT */
        if (sp < DICT_MAXOPS) st[sp++] = d;
    }
    return -1;
hit:
    for (int i = 0; i < sp && i < cap; i++) out[i] = st[i];
    return sp;
}

static int dict_int(const struct fr *b, uint32_t off, uint32_t len, uint32_t op,
                    int32_t *v)
{
    struct dop d[DICT_MAXOPS];
    int n = dict_get(b, off, len, op, d, DICT_MAXOPS);
    if (n < 1) return -1;
    *v = dop_int(&d[0]);
    return 0;
}

/* One font's Private DICT: local subrs, the two widths, and (CFF2) vsindex. */
struct privdict { uint32_t subrs; int32_t defw, nomw; int vsindex; };

static void read_private(const struct fr *b, uint32_t poff, uint32_t plen,
                         struct privdict *pv)
{
    pv->subrs = 0; pv->defw = 0; pv->nomw = 0; pv->vsindex = 0;
    if (!plen || !fr_ok(b, poff, plen)) return;
    int32_t v;
    if (dict_int(b, poff, plen, 20, &v) == 0) pv->defw = v;
    if (dict_int(b, poff, plen, 21, &v) == 0) pv->nomw = v;
    if (dict_int(b, poff, plen, 22, &v) == 0 && v >= 0) pv->vsindex = v;   /* CFF2 */
    if (dict_int(b, poff, plen, 19, &v) == 0 && v > 0) {
        uint64_t s = (uint64_t)poff + (uint32_t)v;   /* Subrs offset is from the Private DICT */
        if (s < b->len) pv->subrs = (uint32_t)s;
    }
}

/* ------------------------------------------------------- charset / FDSelect -- */

int cff_glyph_sid(const struct cff_font *f, int gid)
{
    struct fr b = { f->data, f->len };
    if (gid < 0 || gid >= f->nglyphs) return -1;
    if (gid == 0) return 0;                              /* .notdef */
    if (f->charset <= 2) {
        /* Predefined. ISOAdobe (0) is the identity over the standard strings;
         * Expert (1) and ExpertSubset (2) are tables we do not carry. */
        return (f->charset == 0) ? gid : -1;
    }
    uint32_t fmt = fr_u8(&b, f->charset);
    if (fmt == 0) {
        uint32_t p = f->charset + 1 + (uint32_t)(gid - 1) * 2;
        if (!fr_ok(&b, p, 2)) return -1;
        return (int)fr_u16(&b, p);
    }
    if (fmt == 1 || fmt == 2) {
        uint32_t step = (fmt == 1) ? 3u : 4u;
        uint32_t p = f->charset + 1;
        int g = 1;
        while (g < f->nglyphs && fr_ok(&b, p, step)) {
            uint32_t first = fr_u16(&b, p);
            uint32_t nleft = (fmt == 1) ? fr_u8(&b, p + 2) : fr_u16(&b, p + 2);
            if (gid <= g + (int)nleft) return (int)(first + (uint32_t)(gid - g));
            g += (int)nleft + 1;
            p += step;
        }
    }
    return -1;
}

int cff_sid_glyph(const struct cff_font *f, int sid)
{
    if (sid < 0) return -1;
    for (int g = 0; g < f->nglyphs; g++)
        if (cff_glyph_sid(f, g) == sid) return g;
    return -1;
}

/* FD index of a glyph in a CID-keyed font (0 when there is no FDSelect). */
static int fd_of(const struct cff_font *f, int gid)
{
    struct fr b = { f->data, f->len };
    if (!f->fdselect) return 0;
    uint32_t fmt = fr_u8(&b, f->fdselect);
    if (fmt == 0) {
        uint32_t p = f->fdselect + 1 + (uint32_t)gid;
        if (!fr_ok(&b, p, 1)) return 0;
        return (int)fr_u8(&b, p);
    }
    if (fmt == 3) {
        uint32_t nr = fr_u16(&b, f->fdselect + 1);
        uint32_t p = f->fdselect + 3;
        if (!nr || !fr_ok(&b, p, nr * 3u + 2u)) return 0;
        /* Ranges are sorted and contiguous, with a sentinel first-glyph after
         * the last range, so range i covers [first[i], first[i+1]). */
        uint32_t lo = 0, hi = nr;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t first = fr_u16(&b, p + mid * 3);
            uint32_t next  = fr_u16(&b, p + (mid + 1) * 3);
            if ((uint32_t)gid < first) { hi = mid; continue; }
            if ((uint32_t)gid >= next) { lo = mid + 1; continue; }
            return (int)fr_u8(&b, p + mid * 3 + 2);
        }
    }
    return 0;
}

/* CFF2: how many variation regions the ItemVariationData at `vsindex` uses.
 * `blend` consumes num_blends * nregions delta operands, so getting this wrong
 * desynchronises the whole charstring. */
static int cff2_regions(const struct cff_font *f, int vsindex)
{
    struct fr b = { f->data, f->len };
    if (!f->vstore || vsindex < 0) return 0;
    uint32_t ivs = f->vstore + 2;                   /* skip the uint16 length */
    if (!fr_ok(&b, ivs, 8) || fr_u16(&b, ivs) != 1) return 0;
    uint32_t cnt = fr_u16(&b, ivs + 6);
    if ((uint32_t)vsindex >= cnt) return 0;
    uint32_t rel = fr_u32(&b, ivs + 8 + (uint32_t)vsindex * 4);
    uint64_t ivd = (uint64_t)ivs + rel;
    if (ivd + 6 > b.len) return 0;
    return (int)fr_u16(&b, (uint32_t)ivd + 4);
}

/* --------------------------------------------------------------- parse -- */

int cff_parse(const uint8_t *data, uint32_t len, uint32_t off, uint32_t size,
              struct cff_font *f)
{
    struct fr b = { data, len };
    if (!data || !fr_ok(&b, off, 4)) return -1;
    /* size is advisory (the table directory's length); clamp it to the blob. */
    if (!size || size > len - off) size = len - off;

    for (int i = 0; i < 4; i++) f->font_bbox[i] = 0;
    f->data = data; f->len = len; f->base = off; f->size = size;
    f->charstring_type = 2; f->upem = 1000; f->is_cid = 0;
    f->charstrings = f->gsubrs = f->strings = f->charset = 0;
    f->fdarray = f->fdselect = f->vstore = f->subrs = 0;
    f->default_width = f->nominal_width = 0;
    f->nglyphs = 0;

    uint32_t major = fr_u8(&b, off);
    uint32_t hdrsize = fr_u8(&b, off + 2);
    f->is_cff2 = (major == 2);
    if (major != 1 && major != 2) return -1;
    if (hdrsize < 4) return -1;

    uint32_t topoff, toplen;
    if (f->is_cff2) {
        /* CFF2 has no Name/String INDEX: header, Top DICT (a plain byte range),
         * then the Global Subr INDEX. */
        uint32_t tdl = fr_u16(&b, off + 3);
        topoff = off + hdrsize; toplen = tdl;
        if (!toplen || !fr_ok(&b, topoff, toplen)) return -1;
        struct cff_index gs;
        if (idx_read(&b, topoff + toplen, 1, &gs) == 0 && gs.count) f->gsubrs = gs.start;
    } else {
        struct cff_index names, tops, strs, gsub;
        if (idx_read(&b, off + hdrsize, 0, &names)) return -1;
        if (idx_read(&b, names.end, 0, &tops)) return -1;
        if (idx_read(&b, tops.end, 0, &strs)) return -1;
        if (idx_read(&b, strs.end, 0, &gsub)) return -1;
        f->strings = strs.count ? strs.start : 0;
        f->gsubrs  = gsub.count ? gsub.start : 0;
        if (idx_get(&b, &tops, 0, &topoff, &toplen)) return -1;   /* face 0 */
    }

    struct dop d[DICT_MAXOPS];
    int32_t v;

    if (dict_int(&b, topoff, toplen, 0x0C06, &v) == 0) f->charstring_type = v;
    if (f->charstring_type != 2) return -1;          /* Type 1 charstrings: not interpreted */

    /* FontMatrix[0] gives units-per-em; the default 0.001 means upem 1000. */
    int n = dict_get(&b, topoff, toplen, 0x0C07, d, DICT_MAXOPS);
    if (n >= 1 && d[0].num > 0 && d[0].exp < 0 && -d[0].exp <= 18) {
        int64_t den = d[0].num, num10 = ipow10(-d[0].exp);
        int64_t u = (num10 + den / 2) / den;
        if (u > 0 && u <= 16384) f->upem = (int)u;
    }

    n = dict_get(&b, topoff, toplen, 5, d, DICT_MAXOPS);
    if (n >= 4) for (int i = 0; i < 4; i++) f->font_bbox[i] = dop_int(&d[i]);

    if (dict_get(&b, topoff, toplen, 0x0C1E, d, DICT_MAXOPS) >= 0) f->is_cid = 1;

    if (dict_int(&b, topoff, toplen, 17, &v) != 0) return -1;    /* CharStrings */
    if (v <= 0 || (uint64_t)off + (uint32_t)v >= len) return -1;
    f->charstrings = off + (uint32_t)v;

    struct cff_index cs;
    if (idx_read(&b, f->charstrings, f->is_cff2, &cs)) return -1;
    if (cs.count == 0 || cs.count > 65535) return -1;
    f->nglyphs = (int)cs.count;

    if (!f->is_cff2 && dict_int(&b, topoff, toplen, 15, &v) == 0) {
        if (v >= 0 && v <= 2) f->charset = (uint32_t)v;
        else if ((uint64_t)off + (uint32_t)v < len) f->charset = off + (uint32_t)v;
    }

    if (dict_int(&b, topoff, toplen, 0x0C24, &v) == 0 && v > 0 &&
        (uint64_t)off + (uint32_t)v < len) f->fdarray = off + (uint32_t)v;
    if (dict_int(&b, topoff, toplen, 0x0C25, &v) == 0 && v > 0 &&
        (uint64_t)off + (uint32_t)v < len) f->fdselect = off + (uint32_t)v;
    if (f->is_cff2 && dict_int(&b, topoff, toplen, 24, &v) == 0 && v > 0 &&
        (uint64_t)off + (uint32_t)v < len) f->vstore = off + (uint32_t)v;

    /* Private DICT: [size, offset]. CFF2 has none at the top level -- there the
     * Private DICTs hang off FDArray, which is mandatory. */
    n = dict_get(&b, topoff, toplen, 18, d, DICT_MAXOPS);
    if (n >= 2) {
        uint32_t plen = (uint32_t)dop_int(&d[0]);
        int32_t po = dop_int(&d[1]);
        if (po > 0 && (uint64_t)off + (uint32_t)po < len) {
            struct privdict pv;
            read_private(&b, off + (uint32_t)po, plen, &pv);
            f->subrs = pv.subrs;
            f->default_width = pv.defw;
            f->nominal_width = pv.nomw;
        }
    }
    return 0;
}

/* -------------------------------------------------- charstring interpreter -- */

struct t2 {
    const struct cff_font *f;
    struct fr b;
    struct fp_path *p;
    int32_t st[CFF_STACK];
    int sp;
    int32_t x, y;                /* 16.16 current point */
    int32_t width;               /* 16.16 width delta, valid when width_set */
    int nstems;
    int have_width, want_width, width_set;
    int open;
    int32_t trans[TRANS_N];
    struct cff_index gsub, lsub;
    int gbias, lbias;
    int32_t nominal_w, default_w;
    int nregions;                /* CFF2 blend regions for the current vsindex */
    /* seac request produced by the 4-argument endchar */
    int seac;
    int32_t adx, ady; int bchar, achar;
    int err;
};

static void t2_close(struct t2 *c)
{
    if (c->open) { fp_close(c->p); c->open = 0; }
}

static void t2_moveto(struct t2 *c, int32_t x, int32_t y)
{
    t2_close(c);
    c->x = x; c->y = y;
    fp_move(c->p, fixround(x), fixround(y));
    c->open = 1;
}

static void t2_lineto(struct t2 *c, int32_t x, int32_t y)
{
    if (!c->open) { fp_move(c->p, fixround(c->x), fixround(c->y)); c->open = 1; }
    c->x = x; c->y = y;
    fp_line(c->p, fixround(x), fixround(y));
}

static void t2_curveto(struct t2 *c, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                       int32_t ex, int32_t ey)
{
    if (!c->open) { fp_move(c->p, fixround(c->x), fixround(c->y)); c->open = 1; }
    c->x = ex; c->y = ey;
    fp_cubic(c->p, fixround(ax), fixround(ay), fixround(bx), fixround(by),
             fixround(ex), fixround(ey));
}

/* Relative cubic: three successive deltas from the current point. */
static void t2_rrcurve(struct t2 *c, int32_t dx1, int32_t dy1, int32_t dx2, int32_t dy2,
                       int32_t dx3, int32_t dy3)
{
    int32_t ax = c->x + dx1, ay = c->y + dy1;
    int32_t bx = ax + dx2,   by = ay + dy2;
    t2_curveto(c, ax, ay, bx, by, bx + dx3, by + dy3);
}

/* The FIRST stack-clearing operator may carry a leading width. `nargs_even` is 1
 * for the stem/mask operators (which take an even count of arguments); otherwise
 * `expect` is the operator's own argument count and a width shows up as one
 * extra. Removes the width from the bottom of the stack when present. */
static void t2_width(struct t2 *c, int expect, int nargs_even)
{
    if (c->have_width) return;
    c->have_width = 1;
    if (!c->want_width) return;
    int extra = nargs_even ? (c->sp & 1) : (c->sp > expect);
    if (extra && c->sp > 0) {
        c->width = c->st[0];
        c->width_set = 1;
        for (int i = 1; i < c->sp; i++) c->st[i - 1] = c->st[i];
        c->sp--;
    }
}

static int t2_run(struct t2 *c, uint32_t off, uint32_t len, int depth);

/* Run subroutine `idx` (already unbiased) of the given INDEX. */
static int t2_call(struct t2 *c, const struct cff_index *ix, int idx, int depth)
{
    uint32_t o, l;
    if (idx < 0 || idx_get(&c->b, ix, (uint32_t)idx, &o, &l)) { c->err = 1; return -1; }
    return t2_run(c, o, l, depth + 1);
}

/* Returns 1 when execution must stop (endchar, or an error), 0 on `return`. */
static int t2_run(struct t2 *c, uint32_t off, uint32_t len, int depth)
{
    if (depth > SUBR_DEPTH) { c->err = 1; return 1; }
    uint32_t p = off, end = off + len;
    if (!fr_ok(&c->b, off, len)) { c->err = 1; return 1; }

    while (p < end) {
        uint32_t b0 = fr_u8(&c->b, p);

        /* ---- operands ---- */
        if (b0 >= 32 || b0 == 28) {
            int32_t v;
            if (b0 == 28) { if (p + 3 > end) { c->err = 1; return 1; }
                            v = (int32_t)(int16_t)(uint16_t)fr_u16(&c->b, p + 1) * FIX_ONE; p += 3; }
            else if (b0 <= 246) { v = ((int32_t)b0 - 139) * FIX_ONE; p += 1; }
            else if (b0 <= 250) { if (p + 2 > end) { c->err = 1; return 1; }
                                  v = (((int32_t)b0 - 247) * 256 + (int32_t)fr_u8(&c->b, p + 1) + 108) * FIX_ONE; p += 2; }
            else if (b0 <= 254) { if (p + 2 > end) { c->err = 1; return 1; }
                                  v = (-((int32_t)b0 - 251) * 256 - (int32_t)fr_u8(&c->b, p + 1) - 108) * FIX_ONE; p += 2; }
            else { if (p + 5 > end) { c->err = 1; return 1; }
                   v = (int32_t)fr_u32(&c->b, p + 1); p += 5; }   /* 255: already 16.16 */
            if (c->sp < CFF_STACK) c->st[c->sp++] = v;
            else { c->err = 1; return 1; }
            continue;
        }

        p += 1;
        switch (b0) {

        case 1: case 3: case 18: case 23:            /* hstem vstem hstemhm vstemhm */
            t2_width(c, 0, 1);
            c->nstems += c->sp / 2;
            c->sp = 0;
            break;

        case 19: case 20: {                          /* hintmask / cntrmask */
            /* Operands still on the stack here are an implicit vstem. */
            t2_width(c, 0, 1);
            c->nstems += c->sp / 2;
            c->sp = 0;
            uint32_t nb = ((uint32_t)c->nstems + 7) / 8;
            if (nb > end - p) { c->err = 1; return 1; }
            p += nb;                                 /* the mask itself is discarded */
            break;
        }

        case 21:                                     /* rmoveto */
            t2_width(c, 2, 0);
            if (c->sp < 2) { c->err = 1; return 1; }
            t2_moveto(c, c->x + c->st[0], c->y + c->st[1]);
            c->sp = 0;
            break;

        case 22:                                     /* hmoveto */
            t2_width(c, 1, 0);
            if (c->sp < 1) { c->err = 1; return 1; }
            t2_moveto(c, c->x + c->st[0], c->y);
            c->sp = 0;
            break;

        case 4:                                      /* vmoveto */
            t2_width(c, 1, 0);
            if (c->sp < 1) { c->err = 1; return 1; }
            t2_moveto(c, c->x, c->y + c->st[0]);
            c->sp = 0;
            break;

        case 5:                                      /* rlineto */
            for (int i = 0; i + 1 < c->sp; i += 2)
                t2_lineto(c, c->x + c->st[i], c->y + c->st[i + 1]);
            c->sp = 0;
            break;

        case 6: case 7: {                            /* hlineto / vlineto */
            int horiz = (b0 == 6);
            for (int i = 0; i < c->sp; i++, horiz = !horiz) {
                if (horiz) t2_lineto(c, c->x + c->st[i], c->y);
                else       t2_lineto(c, c->x, c->y + c->st[i]);
            }
            c->sp = 0;
            break;
        }

        case 8:                                      /* rrcurveto */
            for (int i = 0; i + 5 < c->sp; i += 6)
                t2_rrcurve(c, c->st[i], c->st[i+1], c->st[i+2], c->st[i+3], c->st[i+4], c->st[i+5]);
            c->sp = 0;
            break;

        case 24: {                                   /* rcurveline */
            int i = 0;
            while (c->sp - i >= 8) {
                t2_rrcurve(c, c->st[i], c->st[i+1], c->st[i+2], c->st[i+3], c->st[i+4], c->st[i+5]);
                i += 6;
            }
            if (c->sp - i >= 2) t2_lineto(c, c->x + c->st[i], c->y + c->st[i + 1]);
            c->sp = 0;
            break;
        }

        case 25: {                                   /* rlinecurve */
            int i = 0;
            while (c->sp - i >= 8) {
                t2_lineto(c, c->x + c->st[i], c->y + c->st[i + 1]);
                i += 2;
            }
            if (c->sp - i >= 6)
                t2_rrcurve(c, c->st[i], c->st[i+1], c->st[i+2], c->st[i+3], c->st[i+4], c->st[i+5]);
            c->sp = 0;
            break;
        }

        case 26: {                                   /* vvcurveto: dx1? {dya dxb dyb dyc}+ */
            int i = 0; int32_t dx1 = 0;
            if (c->sp & 1) { dx1 = c->st[0]; i = 1; }
            for (; i + 3 < c->sp; i += 4) {
                t2_rrcurve(c, dx1, c->st[i], c->st[i+1], c->st[i+2], 0, c->st[i+3]);
                dx1 = 0;
            }
            c->sp = 0;
            break;
        }

        case 27: {                                   /* hhcurveto: dy1? {dxa dxb dyb dxc}+ */
            int i = 0; int32_t dy1 = 0;
            if (c->sp & 1) { dy1 = c->st[0]; i = 1; }
            for (; i + 3 < c->sp; i += 4) {
                t2_rrcurve(c, c->st[i], dy1, c->st[i+1], c->st[i+2], c->st[i+3], 0);
                dy1 = 0;
            }
            c->sp = 0;
            break;
        }

        case 30: case 31: {                          /* vhcurveto / hvcurveto */
            int horiz = (b0 == 31);
            int i = 0;
            while (c->sp - i >= 4) {
#ifdef FONT_CONTROL_HV_LAST
                /* NEGATIVE CONTROL (make test-font-control): drop the optional
                 * trailing coordinate of the final curve. This is the classic
                 * way to get hv/vhcurveto wrong, and it is exactly the failure
                 * this suite exists to catch -- every glyph still comes out
                 * looking like a letter, with one curve endpoint in the wrong
                 * place. If the exact path comparison still passes with this
                 * defined, it is not comparing anything. */
                int last5 = 0;
#else
                int last5 = (c->sp - i == 5);
#endif
                int32_t dx1, dy1, dx2, dy2, dx3, dy3;
                if (horiz) {
                    dx1 = c->st[i];   dy1 = 0;
                    dx2 = c->st[i+1]; dy2 = c->st[i+2];
                    dy3 = c->st[i+3]; dx3 = last5 ? c->st[i+4] : 0;
                } else {
                    dx1 = 0;          dy1 = c->st[i];
                    dx2 = c->st[i+1]; dy2 = c->st[i+2];
                    dx3 = c->st[i+3]; dy3 = last5 ? c->st[i+4] : 0;
                }
                t2_rrcurve(c, dx1, dy1, dx2, dy2, dx3, dy3);
                i += 4; horiz = !horiz;
            }
            c->sp = 0;
            break;
        }

        case 10:                                     /* callsubr */
            if (c->sp < 1) { c->err = 1; return 1; }
            if (t2_call(c, &c->lsub, fixround(c->st[--c->sp]) + c->lbias, depth)) return 1;
            break;

        case 29:                                     /* callgsubr */
            if (c->sp < 1) { c->err = 1; return 1; }
            if (t2_call(c, &c->gsub, fixround(c->st[--c->sp]) + c->gbias, depth)) return 1;
            break;

        case 11:                                     /* return */
            return 0;

        case 14:                                     /* endchar */
            if (c->f->is_cff2) { c->err = 1; return 1; }
            /* endchar takes 0 or 4 arguments, so a leading width leaves 1 or 5.
             * The generic even/odd rule would misread the 4-argument seac form. */
            if (!c->have_width) {
                c->have_width = 1;
                if (c->sp == 1 || c->sp == 5) {
                    c->width = c->st[0]; c->width_set = 1;
                    for (int i = 1; i < c->sp; i++) c->st[i - 1] = c->st[i];
                    c->sp--;
                }
            }
            if (c->sp >= 4) {
                c->seac = 1;
                c->adx = c->st[0]; c->ady = c->st[1];
                c->bchar = fixround(c->st[2]); c->achar = fixround(c->st[3]);
            }
            t2_close(c);
            c->sp = 0;
            return 1;

        case 15:                                     /* vsindex (CFF2) */
            if (!c->f->is_cff2 || c->sp < 1) { c->err = 1; return 1; }
            c->nregions = cff2_regions(c->f, fixround(c->st[c->sp - 1]));
            c->sp = 0;
            break;

        case 16: {                                   /* blend (CFF2) */
            if (!c->f->is_cff2 || c->sp < 1) { c->err = 1; return 1; }
            /* Stack: v1..vn, then n*nregions deltas, then n. At the default
             * instance every delta is scaled by zero, so drop them and keep the
             * n default values. */
            int nops = fixround(c->st[--c->sp]);
            if (nops < 0) { c->err = 1; return 1; }
            long drop = (long)nops * c->nregions;
            if (drop + nops > c->sp) { c->err = 1; return 1; }
            c->sp -= (int)drop;
            break;
        }

        case 12: {                                   /* escape */
            if (p >= end) { c->err = 1; return 1; }
            uint32_t b1 = fr_u8(&c->b, p); p += 1;
            switch (b1) {
            case 35:                                 /* flex */
                if (c->sp < 13) { c->err = 1; return 1; }
                t2_rrcurve(c, c->st[0], c->st[1], c->st[2], c->st[3], c->st[4], c->st[5]);
                t2_rrcurve(c, c->st[6], c->st[7], c->st[8], c->st[9], c->st[10], c->st[11]);
                c->sp = 0;
                break;
            case 34: {                               /* hflex */
                if (c->sp < 7) { c->err = 1; return 1; }
                int32_t y0 = c->y;
                t2_rrcurve(c, c->st[0], 0, c->st[1], c->st[2], c->st[3], 0);
                int32_t ax = c->x + c->st[4], ay = c->y;      /* dy4 is 0 */
                int32_t bx = ax + c->st[5],   by = y0;        /* dy5 is -dy2 */
                t2_curveto(c, ax, ay, bx, by, bx + c->st[6], y0);
                c->sp = 0;
                break;
            }
            case 36: {                               /* hflex1 */
                if (c->sp < 9) { c->err = 1; return 1; }
                int32_t y0 = c->y;
                t2_rrcurve(c, c->st[0], c->st[1], c->st[2], c->st[3], c->st[4], 0);
                int32_t ax = c->x + c->st[5], ay = c->y;      /* dy4 is 0 */
                int32_t bx = ax + c->st[6],   by = ay + c->st[7];
                t2_curveto(c, ax, ay, bx, by, bx + c->st[8], y0);
                c->sp = 0;
                break;
            }
            case 37: {                               /* flex1 */
                if (c->sp < 11) { c->err = 1; return 1; }
                int32_t sx = c->x, sy = c->y;
                int32_t dx = 0, dy = 0;
                for (int i = 0; i < 10; i += 2) { dx += c->st[i]; dy += c->st[i + 1]; }
                t2_rrcurve(c, c->st[0], c->st[1], c->st[2], c->st[3], c->st[4], c->st[5]);
                int32_t ax = c->x + c->st[6], ay = c->y + c->st[7];
                int32_t bx = ax + c->st[8],   by = ay + c->st[9];
                int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                int32_t ex, ey;
                if (adx > ady) { ex = bx + c->st[10]; ey = sy; }
                else           { ex = sx;            ey = by + c->st[10]; }
                t2_curveto(c, ax, ay, bx, by, ex, ey);
                c->sp = 0;
                break;
            }
            /* ---- arithmetic / storage. Deprecated and vanishingly rare, but a
             * charstring that uses one and finds it missing draws garbage, so
             * they are implemented rather than skipped. ---- */
            case 3: case 4: {                        /* and / or */
                if (c->sp < 2) { c->err = 1; return 1; }
                int32_t a = c->st[c->sp-2], bb = c->st[c->sp-1]; c->sp -= 2;
                c->st[c->sp++] = ((b1 == 3) ? (a && bb) : (a || bb)) ? FIX_ONE : 0;
                break;
            }
            case 5:                                  /* not */
                if (c->sp < 1) { c->err = 1; return 1; }
                c->st[c->sp-1] = c->st[c->sp-1] ? 0 : FIX_ONE;
                break;
            case 9:                                  /* abs */
                if (c->sp < 1) { c->err = 1; return 1; }
                if (c->st[c->sp-1] < 0) c->st[c->sp-1] = -c->st[c->sp-1];
                break;
            case 10:                                 /* add */
                if (c->sp < 2) { c->err = 1; return 1; }
                c->st[c->sp-2] += c->st[c->sp-1]; c->sp--;
                break;
            case 11:                                 /* sub */
                if (c->sp < 2) { c->err = 1; return 1; }
                c->st[c->sp-2] -= c->st[c->sp-1]; c->sp--;
                break;
            case 12:                                 /* div */
                if (c->sp < 2) { c->err = 1; return 1; }
                c->st[c->sp-2] = c->st[c->sp-1]
                    ? (int32_t)(((int64_t)c->st[c->sp-2] << 16) / c->st[c->sp-1]) : 0;
                c->sp--;
                break;
            case 14:                                 /* neg */
                if (c->sp < 1) { c->err = 1; return 1; }
                c->st[c->sp-1] = -c->st[c->sp-1];
                break;
            case 15:                                 /* eq */
                if (c->sp < 2) { c->err = 1; return 1; }
                c->st[c->sp-2] = (c->st[c->sp-2] == c->st[c->sp-1]) ? FIX_ONE : 0;
                c->sp--;
                break;
            case 18:                                 /* drop */
                if (c->sp > 0) c->sp--;
                break;
            case 20: {                               /* put */
                if (c->sp < 2) { c->err = 1; return 1; }
                int i = fixround(c->st[c->sp-1]); c->sp -= 2;
                if (i >= 0 && i < TRANS_N) c->trans[i] = c->st[c->sp];
                break;
            }
            case 21: {                               /* get */
                if (c->sp < 1) { c->err = 1; return 1; }
                int i = fixround(c->st[c->sp-1]);
                c->st[c->sp-1] = (i >= 0 && i < TRANS_N) ? c->trans[i] : 0;
                break;
            }
            case 22: {                               /* ifelse */
                if (c->sp < 4) { c->err = 1; return 1; }
                int32_t s1 = c->st[c->sp-4], s2 = c->st[c->sp-3];
                int32_t v1 = c->st[c->sp-2], v2 = c->st[c->sp-1];
                c->sp -= 3;
                c->st[c->sp-1] = (v1 <= v2) ? s1 : s2;
                break;
            }
            case 23:                                 /* random */
                /* There is no entropy in a rasterizer, and a random outline
                 * would defeat the glyph cache. A fixed 0.5 keeps the stack
                 * discipline, which is the only thing that matters here. */
                if (c->sp >= CFF_STACK) { c->err = 1; return 1; }
                c->st[c->sp++] = FIX_ONE / 2;
                break;
            case 24:                                 /* mul */
                if (c->sp < 2) { c->err = 1; return 1; }
                c->st[c->sp-2] = (int32_t)(((int64_t)c->st[c->sp-2] * c->st[c->sp-1]) >> 16);
                c->sp--;
                break;
            case 26: {                               /* sqrt (Newton on 16.16) */
                if (c->sp < 1) { c->err = 1; return 1; }
                int32_t a = c->st[c->sp-1];
                if (a <= 0) { c->st[c->sp-1] = 0; break; }
                int64_t r = (a > FIX_ONE) ? a : FIX_ONE;
                for (int it = 0; it < 24; it++) r = (r + (((int64_t)a << 16) / r)) / 2;
                c->st[c->sp-1] = (int32_t)r;
                break;
            }
            case 27:                                 /* dup */
                if (c->sp < 1 || c->sp >= CFF_STACK) { c->err = 1; return 1; }
                c->st[c->sp] = c->st[c->sp-1]; c->sp++;
                break;
            case 28: {                               /* exch */
                if (c->sp < 2) { c->err = 1; return 1; }
                int32_t t = c->st[c->sp-1];
                c->st[c->sp-1] = c->st[c->sp-2]; c->st[c->sp-2] = t;
                break;
            }
            case 29: {                               /* index */
                if (c->sp < 1) { c->err = 1; return 1; }
                int i = fixround(c->st[c->sp-1]);
                if (i < 0) i = 0;
                c->st[c->sp-1] = (i < c->sp - 1) ? c->st[c->sp - 2 - i] : 0;
                break;
            }
            case 30: {                               /* roll */
                if (c->sp < 2) { c->err = 1; return 1; }
                int j = fixround(c->st[c->sp-1]);
                int nn = fixround(c->st[c->sp-2]);
                c->sp -= 2;
                if (nn <= 0 || nn > c->sp) break;
                int base = c->sp - nn;
                j = ((j % nn) + nn) % nn;
                for (int r = 0; r < j; r++) {
                    int32_t last = c->st[base + nn - 1];
                    for (int k = nn - 1; k > 0; k--) c->st[base + k] = c->st[base + k - 1];
                    c->st[base] = last;
                }
                break;
            }
            default:
                /* An unknown operator cannot be skipped safely: how many
                 * operands it eats is unknown, so every later coordinate would
                 * be wrong. Refuse the glyph rather than draw rubbish. */
                c->err = 1;
                return 1;
            }
            break;
        }

        default:
            c->err = 1;
            return 1;
        }
    }
    return 0;
}

/* Standard Encoding: character code -> SID, for the seac form of endchar. Codes
 * 32..126 map to SIDs 1..95 in order; the rest of the mapping is sparse. */
static const struct { uint8_t code; uint8_t sid; } std_enc_hi[] = {
    {161, 96},{162, 97},{163, 98},{164, 99},{165,100},{166,101},{167,102},{168,103},
    {169,104},{170,105},{171,106},{172,107},{173,108},{174,109},{175,110},
    {177,111},{178,112},{179,113},{180,114},{182,115},{183,116},{184,117},
    {185,118},{186,119},{187,120},{188,121},{189,122},{191,123},
    {193,124},{194,125},{195,126},{196,127},{197,128},{198,129},{199,130},
    {200,131},{202,132},{203,133},{205,134},{206,135},{207,136},{208,137},
    {225,138},{227,139},{232,140},{233,141},{234,142},{235,143},{241,144},
    {245,145},{248,146},{249,147},{250,148},{251,149},
};

static int std_code_sid(int code)
{
    if (code >= 32 && code <= 126) return code - 31;      /* space=1 .. asciitilde=95 */
    for (unsigned i = 0; i < sizeof std_enc_hi / sizeof std_enc_hi[0]; i++)
        if (std_enc_hi[i].code == code) return std_enc_hi[i].sid;
    return 0;
}

/* Per-glyph Private DICT: CID fonts pick it through FDSelect/FDArray. */
static void t2_local_subrs(struct t2 *c, int gid)
{
    const struct cff_font *f = c->f;
    struct privdict pv;
    pv.subrs = f->subrs; pv.defw = f->default_width; pv.nomw = f->nominal_width;
    pv.vsindex = 0;
    if (f->fdarray) {
        struct cff_index fda;
        uint32_t o, l;
        if (idx_read(&c->b, f->fdarray, f->is_cff2, &fda) == 0 &&
            idx_get(&c->b, &fda, (uint32_t)fd_of(f, gid), &o, &l) == 0) {
            struct dop d[DICT_MAXOPS];
            int n = dict_get(&c->b, o, l, 18, d, DICT_MAXOPS);
            if (n >= 2) {
                uint32_t plen = (uint32_t)dop_int(&d[0]);
                int32_t po = dop_int(&d[1]);
                if (po > 0 && (uint64_t)f->base + (uint32_t)po < f->len)
                    read_private(&c->b, f->base + (uint32_t)po, plen, &pv);
            }
        }
    }
    c->nominal_w = pv.nomw; c->default_w = pv.defw;
    if (f->is_cff2) c->nregions = cff2_regions(f, pv.vsindex);
    c->lsub.ok = 0; c->lsub.count = 0; c->lbias = 0;
    if (pv.subrs && idx_read(&c->b, pv.subrs, f->is_cff2, &c->lsub) == 0)
        c->lbias = subr_bias(c->lsub.count);
}

static void t2_setup(struct t2 *c, const struct cff_font *f, struct fp_path *p)
{
    c->f = f;
    c->b.d = f->data; c->b.len = f->len;
    c->p = p;
    c->sp = 0; c->x = 0; c->y = 0; c->width = 0;
    c->nstems = 0; c->have_width = 0; c->width_set = 0;
    c->want_width = !f->is_cff2;
    c->open = 0; c->seac = 0; c->err = 0;
    c->adx = c->ady = 0; c->bchar = c->achar = 0;
    c->nregions = 0;
    c->nominal_w = f->nominal_width; c->default_w = f->default_width;
    for (int i = 0; i < TRANS_N; i++) c->trans[i] = 0;
    c->gsub.ok = 0; c->gsub.count = 0; c->gbias = 0;
    if (f->gsubrs && idx_read(&c->b, f->gsubrs, f->is_cff2, &c->gsub) == 0)
        c->gbias = subr_bias(c->gsub.count);
    c->lsub.ok = 0; c->lsub.count = 0; c->lbias = 0;
}

static int cff_run_glyph(struct t2 *c, int gid)
{
    struct cff_index cs;
    uint32_t o, l;
    if (idx_read(&c->b, c->f->charstrings, c->f->is_cff2, &cs)) return -1;
    if (idx_get(&c->b, &cs, (uint32_t)gid, &o, &l)) return -1;
    t2_local_subrs(c, gid);
    t2_run(c, o, l, 0);
    t2_close(c);
    return c->err ? -1 : 0;
}

int cff_glyph_path(const struct cff_font *f, int gid, struct fp_path *p)
{
    if (!f || !f->charstrings || gid < 0 || gid >= f->nglyphs) return -1;
    int n0 = p->n;
    struct t2 c;
    t2_setup(&c, f, p);
    if (cff_run_glyph(&c, gid)) return -1;
    if (!c.seac) return p->overflow ? -1 : 0;

    /* endchar's deprecated accented-character form: draw the base glyph, then
     * the accent glyph shifted by (adx, ady). Both are named by StandardEncoding
     * code and resolved through the charset. */
    int bg = cff_sid_glyph(f, std_code_sid(c.bchar));
    int ag = cff_sid_glyph(f, std_code_sid(c.achar));
    if (bg < 0 || ag < 0 || bg == gid || ag == gid) return -1;
    int dx = fixround(c.adx), dy = fixround(c.ady);

    p->n = n0;
    struct t2 cb;
    t2_setup(&cb, f, p);
    if (cff_run_glyph(&cb, bg) || cb.seac) return -1;
    int mark = p->n;
    struct t2 ca;
    t2_setup(&ca, f, p);
    if (cff_run_glyph(&ca, ag) || ca.seac) return -1;
    for (int i = mark; i < p->n; i++)
        for (int k = 0; k < 3; k++) { p->cmd[i].x[k] += dx; p->cmd[i].y[k] += dy; }

    /* the shift moved points, so the accumulated bounds are stale */
    p->xmin = p->ymin = 0x7FFFFFFF; p->xmax = p->ymax = -0x7FFFFFFF;
    for (int i = n0; i < p->n; i++) {
        int npt = (p->cmd[i].op == FP_CUBIC) ? 3 : (p->cmd[i].op == FP_QUAD) ? 2 :
                  (p->cmd[i].op == FP_CLOSE) ? 0 : 1;
        for (int k = 0; k < npt; k++) fp_bound(p, p->cmd[i].x[k], p->cmd[i].y[k]);
    }
    return p->overflow ? -1 : 0;
}

int cff_glyph_width(const struct cff_font *f, int gid)
{
    if (!f || f->is_cff2 || !f->charstrings || gid < 0 || gid >= f->nglyphs) return -1;
    /* Whether a charstring carries a width can only be learned by running it.
     * The path is a one-command throwaway: the interpreter checks capacity, it
     * does not require the path to fit. */
    struct fp_cmd one;
    struct fp_path p;
    fp_init(&p, &one, (int)sizeof one);
    struct t2 c;
    t2_setup(&c, f, &p);
    struct cff_index cs;
    uint32_t o, l;
    if (idx_read(&c.b, f->charstrings, 0, &cs)) return -1;
    if (idx_get(&c.b, &cs, (uint32_t)gid, &o, &l)) return -1;
    t2_local_subrs(&c, gid);
    t2_run(&c, o, l, 0);
    if (c.err) return -1;
    return c.width_set ? (c.nominal_w + fixround(c.width)) : c.default_w;
}
