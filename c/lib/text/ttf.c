#include "ttf.h"
#include "fontrd.h"

/* From-scratch OpenType reader: table directory, head/hhea/maxp, cmap (formats
 * 4 and 12), hmtx advances, and glyph outlines. Two outline sources are
 * supported -- `glyf` (quadratic, simple + composite, handled here) and `CFF `
 * (Type 2 charstrings, handled by cff.c) -- and both are handed to the
 * rasterizer as a struct fp_path. Big-endian on the wire; we read with explicit
 * byte ops so it works on any host. */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  rs16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint32_t tag4(const char *s)
{ return ((uint32_t)(uint8_t)s[0] << 24) | ((uint8_t)s[1] << 16) | ((uint8_t)s[2] << 8) | (uint8_t)s[3]; }

/* Find a table by tag; returns its offset (0 if absent), and its declared
 * length through `outlen` when asked. */
static uint32_t find_table_len(const uint8_t *d, int len, uint32_t tag, uint32_t *outlen)
{
    if (outlen) *outlen = 0;
    if (len < 12) return 0;
    int n = rd16(d + 4);
    const uint8_t *rec = d + 12;
    for (int i = 0; i < n; i++, rec += 16) {
        if (rec + 16 > d + len) break;
        if (rd32(rec) == tag) {
            uint32_t off = rd32(rec + 8);
            if (off >= (uint32_t)len) return 0;      /* a table that starts past EOF */
            if (outlen) {
                uint32_t l = rd32(rec + 12);
                if (l > (uint32_t)len - off) l = (uint32_t)len - off;   /* clamp, do not trust */
                *outlen = l;
            }
            return off;
        }
    }
    return 0;
}

static uint32_t find_table(const uint8_t *d, int len, uint32_t tag)
{ return find_table_len(d, len, tag, 0); }

int ttf_table(const struct ttf_font *f, uint32_t tag, uint32_t *off, uint32_t *len)
{
    uint32_t l = 0;
    uint32_t o = find_table_len(f->data, f->len, tag, &l);
    if (off) *off = o;
    if (len) *len = l;
    return o ? 0 : -1;
}

/* Choose a Unicode cmap subtable: prefer (3,10) format 12, then (3,1) format 4,
 * then (0,*). Returns the subtable offset (absolute), or 0. */
static uint32_t pick_cmap(const uint8_t *d, int len, uint32_t cmap_off)
{
    if (!cmap_off || cmap_off + 4 > (uint32_t)len) return 0;
    const uint8_t *c = d + cmap_off;
    int n = rd16(c + 2);
    uint32_t best = 0; int best_score = -1;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = c + 4 + i * 8;
        if (e + 8 > d + len) break;
        int plat = rd16(e), enc = rd16(e + 2);
        uint32_t sub = cmap_off + rd32(e + 4);
        if (sub + 2 > (uint32_t)len) continue;
        int fmt = rd16(d + sub);
        int score = -1;
        if (plat == 3 && enc == 10 && fmt == 12) score = 4;
        else if (plat == 0 && fmt == 12)         score = 3;
        else if (plat == 3 && enc == 1 && fmt == 4) score = 2;
        else if (plat == 0 && fmt == 4)          score = 1;
        if (score > best_score) { best_score = score; best = sub; }
    }
    return best;
}

int ttf_parse(const uint8_t *data, int len, struct ttf_font *f)
{
    if (len < 12) return -1;
    uint32_t ver = rd32(data);
    if (ver != 0x00010000 && ver != tag4("true") && ver != tag4("OTTO"))
        return -1;                                  /* 'ttcf': extract a face first */

    for (unsigned i = 0; i < sizeof *f; i++) ((uint8_t *)f)[i] = 0;
    f->data = data; f->len = len;
    f->off_head = find_table(data, len, tag4("head"));
    f->off_hhea = find_table(data, len, tag4("hhea"));
    f->off_maxp = find_table(data, len, tag4("maxp"));
    f->off_hmtx = find_table(data, len, tag4("hmtx"));
    f->off_loca = find_table(data, len, tag4("loca"));
    f->off_glyf = find_table(data, len, tag4("glyf"));
    f->off_cmap = find_table(data, len, tag4("cmap"));
    f->off_cff  = find_table_len(data, len, tag4("CFF "), &f->len_cff);
    if (!f->off_cff) f->off_cff = find_table_len(data, len, tag4("CFF2"), &f->len_cff);
    if (!f->off_head || !f->off_hhea || !f->off_maxp || !f->off_hmtx || !f->off_cmap)
        return -1;

    /* find_table only validates the directory record fits, not that the table
     * offset is in range. Bound the head/hhea/maxp reads below (highest read:
     * head+50, hhea+34, maxp+4, each 2 bytes) before dereferencing. */
    if ((uint64_t)f->off_head + 52 > (uint32_t)len ||
        (uint64_t)f->off_hhea + 36 > (uint32_t)len ||
        (uint64_t)f->off_maxp + 6  > (uint32_t)len) return -1;

    f->units_per_em = rd16(data + f->off_head + 18);
    f->loca_long    = rs16(data + f->off_head + 50);
    f->ascent   = rs16(data + f->off_hhea + 4);
    f->descent  = rs16(data + f->off_hhea + 6);
    f->line_gap = rs16(data + f->off_hhea + 8);
    f->num_hmetrics = rd16(data + f->off_hhea + 34);
    f->num_glyphs   = rd16(data + f->off_maxp + 4);
    f->cmap_sub = pick_cmap(data, len, f->off_cmap);
    if (!f->cmap_sub || !f->units_per_em) return -1;
    if (f->num_hmetrics <= 0) return -1;            /* the spec requires >= 1 */

    /* Pick the outline source. A font that carries both (rare, and only from
     * broken tooling) is read as glyf, because that is what its loca indexes. */
    if (f->off_glyf && f->off_loca) {
        f->outline_fmt = TTF_OUTLINE_GLYF;
    } else if (f->off_cff) {
        if (cff_parse(data, (uint32_t)len, f->off_cff, f->len_cff, &f->cff) != 0)
            return -1;
        f->outline_fmt = TTF_OUTLINE_CFF;
        /* head.unitsPerEm is authoritative in OpenType, but a bare/odd CFF can
         * disagree with its own FontMatrix; trust head and note the CFF value
         * only when head is absurd. */
        if (f->units_per_em <= 0) f->units_per_em = f->cff.upem;
        if (f->num_glyphs > f->cff.nglyphs) f->num_glyphs = f->cff.nglyphs;
    } else if (find_table(data, len, tag4("CBDT")) || find_table(data, len, tag4("sbix"))) {
        /* A bitmap-only face -- NotoColorEmoji is exactly this, and it carries no
         * glyf and no CFF at all. It has no outlines to rasterize, but it does
         * have a cmap, metrics and strikes, so accepting it is what lets an
         * emoji come out as an emoji instead of tofu. */
        f->outline_fmt = TTF_OUTLINE_NONE;
    } else {
        return -1;                                  /* nothing we can draw */
    }

    f->off_gsub = find_table(data, len, tag4("GSUB"));
    f->off_gpos = find_table(data, len, tag4("GPOS"));
    f->off_gdef = find_table(data, len, tag4("GDEF"));
    f->off_kern = find_table(data, len, tag4("kern"));
    f->off_colr = find_table(data, len, tag4("COLR"));
    f->off_cpal = find_table(data, len, tag4("CPAL"));
    f->off_cblc = find_table(data, len, tag4("CBLC"));
    f->off_cbdt = find_table(data, len, tag4("CBDT"));
    f->off_sbix = find_table(data, len, tag4("sbix"));
    f->off_svg  = find_table(data, len, tag4("SVG "));
    f->off_fvar = find_table(data, len, tag4("fvar"));
    f->off_avar = find_table(data, len, tag4("avar"));
    f->off_gvar = find_table(data, len, tag4("gvar"));
    f->off_hvar = find_table(data, len, tag4("HVAR"));
    f->off_os2  = find_table(data, len, tag4("OS/2"));
    f->off_post = find_table(data, len, tag4("post"));
    f->off_vhea = find_table(data, len, tag4("vhea"));
    f->off_vmtx = find_table(data, len, tag4("vmtx"));
    f->off_name = find_table(data, len, tag4("name"));
    return 0;
}

int ttf_advance(const struct ttf_font *f, int gid)
{
    if (f->num_hmetrics <= 0) return 0;             /* malformed: avoid i = -1 OOB */
    int i = (gid >= 0 && gid < f->num_hmetrics) ? gid : f->num_hmetrics - 1;
    uint32_t o = f->off_hmtx + (uint32_t)i * 4;
    if (o + 2 > (uint32_t)f->len) return 0;
    return rd16(f->data + o);
}

/* cmap format 4 lookup (segment mapping). All reads are bounded to the subtable
 * (its `length` field) and the font buffer, so a crafted idRangeOffset/segCount
 * cannot read out of bounds. */
static int cmap4(const struct ttf_font *f, const uint8_t *t, uint32_t cp)
{
    if (cp > 0xFFFF) return 0;
    const uint8_t *f_end = f->data + f->len;
    if (t + 14 > f_end) return 0;                       /* header through segCountX2 + reserved */
    const uint8_t *t_end = t + rd16(t + 2);             /* subtable length */
    if (t_end > f_end) t_end = f_end;
    int segX2 = rd16(t + 6);
    int seg = segX2 / 2;
    const uint8_t *endC = t + 14;
    const uint8_t *startC = endC + segX2 + 2;
    const uint8_t *idDelta = startC + segX2;
    const uint8_t *idRange = idDelta + segX2;
    if (idRange + segX2 > t_end) return 0;              /* the four parallel arrays must fit */
    for (int i = 0; i < seg; i++) {
        if (cp <= rd16(endC + i * 2)) {
            int start = rd16(startC + i * 2);
            if (cp < (uint32_t)start) return 0;
            int ro = rd16(idRange + i * 2);
            if (ro == 0) return (uint16_t)(cp + rs16(idDelta + i * 2));
            const uint8_t *gp = idRange + i * 2 + ro + (cp - start) * 2;
            if (gp < t || gp + 2 > t_end) return 0;     /* font-controlled ro/cp -> guard the read */
            int g = rd16(gp);
            return g ? (uint16_t)(g + rs16(idDelta + i * 2)) : 0;
        }
    }
    return 0;
}

/* cmap format 12 lookup (segmented coverage, full Unicode). ngroups is clamped
 * to what fits the font so a crafted count cannot loop off the end. */
static int cmap12(const struct ttf_font *f, const uint8_t *t, uint32_t cp)
{
    const uint8_t *f_end = f->data + f->len;
    if (t + 16 > f_end) return 0;
    uint32_t ngroups = rd32(t + 12);
    const uint8_t *g = t + 16;
    uint32_t rem = (uint32_t)((f_end - g) / 12);
    if (ngroups > rem) ngroups = rem;
    for (uint32_t i = 0; i < ngroups; i++, g += 12) {
        uint32_t s = rd32(g), e = rd32(g + 4);
        if (cp >= s && cp <= e) return rd32(g + 8) + (cp - s);
    }
    return 0;
}

int ttf_glyph_id(const struct ttf_font *f, uint32_t codepoint)
{
    const uint8_t *t = f->data + f->cmap_sub;
    int fmt = rd16(t);
    if (fmt == 4)  return cmap4(f, t, codepoint);
    if (fmt == 12) return cmap12(f, t, codepoint);
    return 0;
}

/* --- glyph outlines --- */

/* loca: byte offset + length of glyph `gid` within the glyf table. */
static int glyf_loc(const struct ttf_font *f, int gid, uint32_t *off, uint32_t *nextoff)
{
    if (gid < 0 || gid >= f->num_glyphs) return -1;
    const uint8_t *l = f->data + f->off_loca;
    /* bound the two loca entries we read against the file (off_loca <= len was
     * checked in ttf_parse, so the subtraction can't underflow). */
    uint32_t need = f->loca_long ? (uint32_t)gid * 4 + 8 : (uint32_t)gid * 2 + 4;
    if (need > (uint32_t)f->len - f->off_loca) return -1;
    if (f->loca_long) { *off = rd32(l + gid * 4); *nextoff = rd32(l + gid * 4 + 4); }
    else { *off = rd16(l + gid * 2) * 2u; *nextoff = rd16(l + gid * 2 + 2) * 2u; }
    return 0;
}

/* Fixed-point 2.14 (F2Dot14) arithmetic for composite glyphs.
 *
 * These are 64-bit and clamped because the inputs are attacker-controlled: a
 * component's scale is an F2Dot14 in [-2, 2), five levels of nesting multiply
 * those together, and `a * x` then overflows int -- which UBSan caught on a
 * mutated font. The clamp is deliberately far past anything a real glyph
 * reaches (2^22 in 2.14 is a scale of 256); it exists so the arithmetic stays
 * defined, not to rescue a glyph that is already nonsense. */
#define XF_LIM (1 << 22)

static int c214(int64_t v)
{
    v >>= 14;
    if (v >  XF_LIM) v =  XF_LIM;
    if (v < -XF_LIM) v = -XF_LIM;
    return (int)v;
}

static int clamp_off(int64_t v)
{
    if (v >  XF_LIM) v =  XF_LIM;
    if (v < -XF_LIM) v = -XF_LIM;
    return (int)v;
}

/* Transform one point through [a c / b d] + (dx,dy), landing back in a short. */
static short xf(int a, int c, int x, int y, int d)
{ return (short)(c214((int64_t)a * x + (int64_t)c * y) + d); }

/* Emit glyph `gid` into the outline arrays, applying the 2.14 transform
 * [a b / c d] + (dx,dy). Recurses for composites. Returns 0 ok, -1 on overflow.
 * FL is a caller-owned flags scratch of capPts bytes (used only while parsing a
 * simple glyph, so one buffer serves every recursion level). */
static int emit(const struct ttf_font *f, int gid, int depth,
                int a, int b, int c, int d, int dx, int dy,
                short *X, short *Y, uint8_t *ON, uint8_t *FL, int *CE,
                int capPts, int capC, int *npts, int *nc)
{
    if (depth > 5) return -1;
    uint32_t off, nextoff;
    if (glyf_loc(f, gid, &off, &nextoff)) return -1;
    if (nextoff <= off) return 0;                       /* blank glyph */
    const uint8_t *g = f->data + f->off_glyf + off;
    /* This glyph spans [off, nextoff) within glyf; bound every read to it and to
     * the file. off_glyf <= len was checked in ttf_parse. */
    const uint8_t *gend = f->data + f->off_glyf + nextoff;
    if (gend > f->data + f->len) gend = f->data + f->len;
    if (g + 10 > gend) return -1;                       /* need the 10-byte glyph header */
    int ncont = rs16(g);
    const uint8_t *p = g + 10;

    if (ncont >= 0) {                                   /* --- simple glyph --- */
        const uint8_t *endpts = p;
        int base = *npts;
        if (ncont > capC) return -1;
        if (endpts + (uint32_t)ncont * 2 > gend) return -1;
        int gpts = (ncont == 0) ? 0 : rd16(endpts + (ncont - 1) * 2) + 1;
        p = endpts + ncont * 2;
        if (p + 2 > gend) return -1;
        int insLen = rd16(p); p += 2 + insLen;
        if (p > gend) return -1;
        if (*npts + gpts > capPts || *nc + ncont > capC) return -1;

        uint8_t *flags = FL;                        /* capPts bytes; gpts <= capPts was checked above */
        for (int i = 0; i < gpts; ) {
            if (p >= gend) return -1;
            uint8_t fl = *p++; flags[i++] = fl;
            if (fl & 0x08) { if (p >= gend) return -1; int r = *p++; while (r-- && i < gpts) flags[i++] = fl; }
        }
        int xv = 0;
        for (int i = 0; i < gpts; i++) {
            uint8_t fl = flags[i];
            if (fl & 0x02) { if (p >= gend) return -1; int dxv = *p++; xv += (fl & 0x10) ? dxv : -dxv; }
            else if (!(fl & 0x10)) { if (p + 2 > gend) return -1; xv += rs16(p); p += 2; }
            X[base + i] = (short)xv;                    /* raw; transform below */
        }
        int yv = 0;
        for (int i = 0; i < gpts; i++) {
            uint8_t fl = flags[i];
            if (fl & 0x04) { if (p >= gend) return -1; int dyv = *p++; yv += (fl & 0x20) ? dyv : -dyv; }
            else if (!(fl & 0x20)) { if (p + 2 > gend) return -1; yv += rs16(p); p += 2; }
            Y[base + i] = (short)yv;
            ON[base + i] = (uint8_t)(fl & 0x01);
        }
        for (int i = 0; i < gpts; i++) {                /* apply transform */
            int rx = X[base + i], ry = Y[base + i];
            X[base + i] = xf(a, c, rx, ry, dx);
            Y[base + i] = xf(b, d, rx, ry, dy);
        }
        for (int i = 0; i < ncont; i++)
            CE[(*nc) + i] = base + rd16(endpts + i * 2);
        *npts += gpts; *nc += ncont;
        return 0;
    }

    /* --- composite glyph --- */
    for (;;) {
        if (p + 4 > gend) return -1;
        int flags = rd16(p); int cgid = rd16(p + 2); p += 4;
        int arg1, arg2;
        if (flags & 0x0001) { if (p + 4 > gend) return -1; arg1 = rs16(p); arg2 = rs16(p + 2); p += 4; }
        else { if (p + 2 > gend) return -1; arg1 = (signed char)p[0]; arg2 = (signed char)p[1]; p += 2; }
        int ca = 0x4000, cb = 0, cc = 0, cd = 0x4000;   /* 1.0 in 2.14 */
        if (flags & 0x0008) { if (p + 2 > gend) return -1; ca = cd = rs16(p); p += 2; }
        else if (flags & 0x0040) { if (p + 4 > gend) return -1; ca = rs16(p); cd = rs16(p + 2); p += 4; }
        else if (flags & 0x0080) { if (p + 8 > gend) return -1; ca = rs16(p); cb = rs16(p + 2); cc = rs16(p + 4); cd = rs16(p + 6); p += 8; }
        int cdx = (flags & 0x0002) ? arg1 : 0;          /* XY offset (point-match unsupported) */
        int cdy = (flags & 0x0002) ? arg2 : 0;
        /* compose parent [a b c d] with child [ca cb cc cd] (2.14) */
        int na  = c214((int64_t)a * ca + (int64_t)c * cb);
        int nb  = c214((int64_t)b * ca + (int64_t)d * cb);
        int nc2 = c214((int64_t)a * cc + (int64_t)c * cd);
        int nd  = c214((int64_t)b * cc + (int64_t)d * cd);
        int ndx = clamp_off((int64_t)c214((int64_t)a * cdx + (int64_t)c * cdy) + dx);
        int ndy = clamp_off((int64_t)c214((int64_t)b * cdx + (int64_t)d * cdy) + dy);
        if (emit(f, cgid, depth + 1, na, nb, nc2, nd, ndx, ndy, X, Y, ON, FL, CE, capPts, capC, npts, nc))
            return -1;
        if (!(flags & 0x0020)) break;                   /* MORE_COMPONENTS */
    }
    return 0;
}

int ttf_glyph_outline(const struct ttf_font *f, int gid,
                      struct ttf_outline *out, void *scratch, int scratchlen)
{
    if (f->outline_fmt != TTF_OUTLINE_GLYF) return -1;   /* CFF has no point array */
    /* Carve scratch: CE[capC] (int) then X,Y (short) then ON,FL (byte). FL holds
     * the per-glyph flag bytes so emit() needs no on-stack VLA -- kernel thread
     * stacks are far smaller than the worst-case point count. */
    /* Contour capacity was a flat 64, which is not enough: a colour-emoji layer
     * or a dense CJK ideograph runs to 85-90 contours, and every one of those
     * glyphs returned -1 and drew NOTHING. Scale it with the scratch instead --
     * one contour costs 4 bytes here against the ~7 bytes a point costs below,
     * so giving contours 1/64th of the buffer is generous either way. */
    int capC = scratchlen / 64;
    if (capC < 64) capC = 64;
    if (capC > 8192) capC = 8192;
    if (capC * (int)sizeof(int) >= scratchlen) return -1;
    uint8_t *base = (uint8_t *)scratch;
    int *CE = (int *)base;
    uint8_t *rest = base + capC * (int)sizeof(int);
    int restlen = scratchlen - capC * (int)sizeof(int);
    if (restlen < 16) return -1;
    int capPts = restlen / 7;                            /* 2(x)+2(y)+1(on)+1(flags) */
    capPts &= ~1;
    short *X = (short *)rest;
    short *Y = (short *)(rest + capPts * 2);
    uint8_t *ON = rest + capPts * 4;
    uint8_t *FL = rest + capPts * 6;

    out->x = X; out->y = Y; out->on = ON; out->contour_end = CE;
    out->npts = 0; out->ncontours = 0;
    if (emit(f, gid, 0, 0x4000, 0, 0, 0x4000, 0, 0, X, Y, ON, FL, CE, capPts, capC,
             &out->npts, &out->ncontours))
        return -1;

    /* bbox from header (font units) */
    uint32_t off, nextoff;
    if (glyf_loc(f, gid, &off, &nextoff) == 0 && nextoff > off &&
        f->off_glyf + off + 10 <= (uint32_t)f->len) {
        const uint8_t *g = f->data + f->off_glyf + off;
        out->xmin = rs16(g + 2); out->ymin = rs16(g + 4);
        out->xmax = rs16(g + 6); out->ymax = rs16(g + 8);
    } else { out->xmin = out->ymin = out->xmax = out->ymax = 0; }
    return 0;
}

/* --- outline -> drawing commands ---------------------------------------- */

/* One glyf contour as MOVE / LINE / QUAD / CLOSE.
 *
 * TrueType stores a contour as a ring of points, each on- or off-curve, with
 * three things left implicit: the contour may not start on-curve, two adjacent
 * off-curve points imply an on-curve point halfway between them, and the ring
 * closes back to where it started. All three are resolved here so that the
 * rasterizer only ever sees explicit segments. */
static void contour_to_path(struct fp_path *p, const struct ttf_outline *o,
                            int start, int end)
{
    int n = end - start + 1;
    if (n < 2) return;
    int s0 = -1;
    for (int i = start; i <= end; i++) if (o->on[i]) { s0 = i; break; }
    int32_t sx, sy;
    if (s0 < 0) {
        /* an all-off-curve contour (common in CJK): start at the implied
         * midpoint between the last and first points */
        sx = ((int32_t)o->x[start] + o->x[end]) / 2;
        sy = ((int32_t)o->y[start] + o->y[end]) / 2;
        s0 = start;
    } else { sx = o->x[s0]; sy = o->y[s0]; }

    fp_move(p, sx, sy);
    int have_ctrl = 0; int32_t cx = 0, cy = 0;
    for (int j = 1; j <= n; j++) {
        int idx = start + ((s0 - start + j) % n);
        int32_t px = o->x[idx], py = o->y[idx];
        if (o->on[idx]) {
            if (have_ctrl) { fp_quad(p, cx, cy, px, py); have_ctrl = 0; }
            else fp_line(p, px, py);
        } else {
#ifndef FONT_CONTROL_NO_MIDPOINT
            if (have_ctrl) fp_quad(p, cx, cy, (cx + px) / 2, (cy + py) / 2);
#else
            /* NEGATIVE CONTROL (make test-font-control): drop the on-curve point
             * implied between two consecutive off-curve points, keeping only the
             * later control. Letters still come out as letters -- rounder, with
             * one curve missing per pair -- which is precisely the kind of wrong
             * that an "it drew something" test cannot see. */
#endif
            cx = px; cy = py; have_ctrl = 1;
        }
    }
    if (have_ctrl) fp_quad(p, cx, cy, sx, sy);
    else fp_line(p, sx, sy);
    fp_close(p);
}

int ttf_glyph_path(const struct ttf_font *f, int gid, struct fp_path *p,
                   void *scratch, int scratchlen)
{
    if (f->outline_fmt == TTF_OUTLINE_CFF)
        return cff_glyph_path(&f->cff, gid, p);
    if (f->outline_fmt != TTF_OUTLINE_GLYF) return -1;

    struct ttf_outline o;
    if (ttf_glyph_outline(f, gid, &o, scratch, scratchlen)) return -1;
    int start = 0;
    for (int ci = 0; ci < o.ncontours; ci++) {
        int end = o.contour_end[ci];
        if (end < start || end >= o.npts) { start = end + 1; continue; }
        contour_to_path(p, &o, start, end);
        start = end + 1;
    }
    return p->overflow ? -1 : 0;
}
