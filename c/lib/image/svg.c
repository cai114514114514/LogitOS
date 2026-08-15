/* SVG rasterizer, aimed at web icons (GitHub octicons are the reference
 * corpus). Supported subset:
 *   <svg width/height/viewBox>, <g>/<svg> nesting with inherited paint,
 *   <path d> commands M L H V C S Q T A Z (absolute + relative),
 *   <rect>, <circle>, <ellipse>;
 *   fill/stroke = #rgb/#rrggbb/#rgba/#rrggbbaa/rgb()/rgba()/keyword/
 *   currentColor(-> black)/none, fill-opacity/stroke-opacity/opacity,
 *   fill-rule=evenodd (nonzero default), stroke-width. Anything unsupported
 *   (transform, style, defs, text, dasharray, ...) is skipped, never fatal;
 *   malformed input must never crash or overrun.
 *
 * GEOMETRY GOES THROUGH OPEN LOGIT (c/lib/gfx), NOT A HAND-ROLLED FILLER.
 * This file used to carry its own scanline filler (one sample per scanline,
 * no antialiasing at all) and its own double-precision libm (an 80-iteration
 * Newton sqrt, an 11th-order Taylor sin) -- floating point in front of
 * attacker-shaped bytes, reachable from disk via SYS_IMG_DECODE and the
 * wallpaper loader, and it duplicated a rasterizer this tree already has one
 * of. Every shape here is now built as a struct gfx_path (move/line/quad/
 * cubic/close) and handed to gfx_fill -- so an SVG icon is antialiased by the
 * exact same coverage rule as every rounded rect and every glyph in the
 * system, and stroking (stroke="..."/stroke-width) is gfx_stroke_path
 * turning the same path into its outline, not a second fill algorithm.
 *
 * INTEGER ONLY, deliberately, and not merely "no doubles left over from the
 * kernel days": this file is RING-3 ONLY now (see the Makefile's C_SRC
 * filter-out list) but the SAME source still compiles for the host test and
 * for three different ring-3 binaries, none of which owe it an FPU. Every
 * coordinate is 24.8 fixed point in DEVICE pixels -- the same format
 * gfx_path.c stores paths in -- so a parsed coordinate is fed to
 * gfx_move_to/gfx_line_to/gfx_quad_to/gfx_cubic_to with no conversion at
 * all, and gfx_path.c's own adaptive flattening does the curve subdivision
 * this file used to do by hand (cub4/quad are gone; gfx_cubic_to/
 * gfx_quad_to already flatten in device space against a device-space
 * tolerance, which is strictly better than the fixed depth-12 heuristic they
 * replace). The one piece of math genuinely local to SVG -- the elliptical
 * arc's endpoint-to-center parameterization (the `A` path command) -- still
 * needs sin/cos/atan2 of its own; sin/cos are gfx_sin/gfx_cos (the engine's
 * quarter-wave table), and atan2 is a small fixed-point port of the same
 * bounded-error rational approximation this file always used, now returning
 * degrees*256 so it feeds gfx_sin/gfx_cos directly with no radian<->degree
 * conversion in between. Every multiply/divide that combines two fixed-point
 * quantities goes through imul_shr16/imuldiv, which widen to __int128 before
 * the divide -- rx^2*ry^2 alone can hit ~7.5e22 at this file's own 2048px
 * canvas cap, which does not fit in int64. That is not a defensive nicety,
 * it is required for the arc math not to wrap.
 *
 * NO PRIVILEGED SHAPES EITHER: <circle>/<ellipse> build a gfx_path_ellipse
 * (4 cubics, exact to the tolerance the engine already ships) instead of the
 * old N-gon-by-sampling-cos/sin loop, and get antialiasing and correct
 * stroke closure for free -- gfx_path_ellipse's last curve ends exactly on
 * its first point, which is what lets gfx_stroke_path treat it as closed
 * (see gfx_stroke.c's file comment on how closure is detected). <rect> is
 * NOT built via gfx_path_rect for the same reason working the other way:
 * gfx_path_rect stops one edge short of closing (gfx_close() puts the pen
 * back but emits no point), so a rect built that way would stroke as an open
 * four-point polyline with two caps at the top-left corner instead of a
 * join -- wrong for a shape that is always closed in SVG. build_rect() below
 * emits the closing edge explicitly instead.
 */
#include "gfx.h"
#include "img.h"

void *kmalloc(unsigned long);
void  kfree(void *);

/* ---- fixed-point arithmetic the arc math and the viewBox transform share --
 * Every product here goes through __int128 before narrowing, and every
 * result is clamped to +-8,000,000 (24.8 that's about +-31,250 device
 * pixels, or 16.16 that's about +-122 -- either way, wildly more headroom
 * than any real coordinate or trig ratio ever needs and still small enough
 * that a handful of clamped values can be summed with no int32 overflow
 * downstream). That is the whole overflow story for this file: nothing past
 * this point needs its own bounds check. */
static long long clampfx(long long v)
{
    if (v > 8000000) return 8000000;
    if (v < -8000000) return -8000000;
    return v;
}

/* a is whatever scale the caller means it to be (24.8 device, or a 16.16
 * dimensionless ratio); b is always a 16.16 dimensionless factor (a trig
 * value, a uniform scale). Result keeps a's scale -- this is the one
 * operation every triple product below (rx*cos*cos_phi, ...) is built from. */
static long long imul_shr16(long long a, long long b)
{
    return clampfx((long long)(((__int128)a * b) >> 16));
}

/* a*b/c, any consistent scale, __int128 intermediate so a*b never wraps
 * before the divide (rx^2*ry^2-style products get here at up to ~7.5e22). */
static long long imuldiv(long long a, long long b, long long c)
{
    if (c == 0) return 0;
    return clampfx((long long)(((__int128)a * b) / c));
}

/* Narrow a possibly-huge __int128 ratio (already scaled *65536) to what
 * gfx_isqrt (unsigned long long in, unsigned long long out) can take without
 * the <<16 below wrapping 64 bits. The cap is absurdly generous for any
 * legitimate lam/co ratio (both are close to 1 by construction for a valid
 * ellipse) -- it exists only so adversarial rx/ry near the arc_to() epsilon
 * floor can't turn a division into undefined behaviour two lines later. */
static unsigned long long sqrt_ratio16(__int128 ratio16)
{
    const __int128 CAP = (__int128)1 << 40;
    if (ratio16 < 0) ratio16 = 0;
    if (ratio16 > CAP) ratio16 = CAP;
    return gfx_isqrt((unsigned long long)ratio16 << 16);
}

/* atan(t) for |t|<=1, radians ~= |t|*(pi/4 + 0.273*(1-|t|)) is a standard
 * bounded-error rational approximation (~0.004 rad worst case) -- this file
 * has always used it, just in double before. In degrees*256 the constants
 * become 45*256 and round(0.273 * 180/pi * 256) = 4004. */
static long long iatan1_deg256(long long t16)
{
    long long at = t16 < 0 ? -t16 : t16;
    long long inner = 45 * 256 + imuldiv(4004, 65536 - at, 65536);
    long long r = imuldiv(at, inner, 65536);
    return t16 < 0 ? -r : r;
}

static int iatan2_deg256(long long y, long long x)
{
    long long ay = y < 0 ? -y : y, ax = x < 0 ? -x : x;
    if (ay == 0 && ax == 0) return 0;
    long long r;
    if (ax >= ay) {
        r = iatan1_deg256(imuldiv(y, 65536, x));
        if (x < 0) r += y >= 0 ? 180 * 256 : -180 * 256;
    } else {
        r = (y > 0 ? 90 * 256 : -90 * 256) - iatan1_deg256(imuldiv(x, 65536, y));
    }
    return (int)r;
}

/* tan of a degrees*256 angle, 16.16, via the engine's own sin/cos -- this is
 * the only trig SVG needs that gfx_math.c doesn't already export. */
static long long itan_deg256(int deg256)
{
    long long c = gfx_cos(deg256);
    if (c == 0) return 0;
    return imuldiv(gfx_sin(deg256), 65536, c);
}

/* ---- lexer helpers ---- */
static int is_ws(int c)   { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
static int is_alpha(int c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(int c){ return c >= '0' && c <= '9'; }
static int is_name(int c) { return is_alpha(c) || is_digit(c) || c == '-' || c == '_' || c == ':' || c == '.'; }

static int str_eq(const uint8_t *p, int n, const char *s)
{
    int i = 0;
    while (s[i]) { if (i >= n || p[i] != (uint8_t)s[i]) return 0; i++; }
    return i == n;
}

/* Parse a decimal (with optional exponent) into a 24.8 fixed value. Integer
 * and fractional digits accumulate SEPARATELY (ip, frac_num/frac_den) and
 * are combined once at the end -- accumulating a running fixed-point
 * fraction digit by digit (as the old double parser effectively did via
 * `frac *= 0.1`) compounds rounding error digit by digit instead of paying
 * it once. Stops at the first character that cannot extend the number, so
 * "1.2.3" reads as 1.2 then .3 and "10-3" as 10 then -3, matching SVG's
 * comma/sign-optional number list grammar. */
static int pnum_fx(const uint8_t **pp, const uint8_t *end, int *out)
{
    const uint8_t *p = *pp;
    int sign = 1, any = 0;
    if (p < end && (*p == '-' || *p == '+')) { if (*p == '-') sign = -1; p++; }
    long long ip = 0;
    while (p < end && is_digit(*p)) {
        any = 1;
        if (ip < 1000000000000LL) ip = ip * 10 + (*p - '0');   /* far past any real coord; further digits are noise */
        p++;
    }
    long long frac_num = 0, frac_den = 1;
    if (p < end && *p == '.') {
        p++;
        while (p < end && is_digit(*p)) {
            any = 1;
            if (frac_den < 100000000LL) { frac_num = frac_num * 10 + (*p - '0'); frac_den *= 10; }
            p++;
        }
    }
    if (!any) return -1;
    long long val = ip * 256 + (frac_num * 256) / frac_den;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const uint8_t *q = p + 1;
        int es = 1, eany = 0;
        long long e = 0;
        if (q < end && (*q == '-' || *q == '+')) { if (*q == '-') es = -1; q++; }
        while (q < end && is_digit(*q)) { eany = 1; if (e < 40) e = e * 10 + (*q - '0'); q++; }
        if (eany) {
            p = q;
            while (e-- > 0) {
                if (es > 0) { if (val > 8000000) break; val *= 10; }
                else val /= 10;
            }
        }
    }
    val = clampfx(val);
    *out = (int)(sign < 0 ? -val : val);
    *pp = p;
    return 0;
}

/* ---- XML tag scanning (pure byte-level parsing; no arithmetic below needs
 * fixed point and none of this changed when the rasterizer did) ---- */
struct tag {
    const uint8_t *name; int nlen;
    const uint8_t *at, *aend;                       /* attribute span */
    int closing, selfclose;
};

/* Cursor is at '<'; parses one tag (or returns -1 on truncation/garbage)
 * and advances the cursor past '>'. */
static int parse_tag(const uint8_t **pp, const uint8_t *end, struct tag *t)
{
    const uint8_t *p = *pp + 1;
    t->closing = 0;
    if (p < end && *p == '/') { t->closing = 1; p++; }
    const uint8_t *ns = p;
    while (p < end && is_name(*p)) p++;
    if (p == ns) return -1;
    t->name = ns; t->nlen = p - ns;
    t->at = p;
    int q = 0, lastc = 0;
    while (p < end) {
        int c = *p;
        if (q) { if (c == q) q = 0; p++; continue; }
        if (c == '"' || c == '\'') { q = c; p++; continue; }
        if (c == '>') break;
        if (!is_ws(c)) lastc = c;
        p++;
    }
    if (p >= end) return -1;                        /* no '>' before EOF */
    t->aend = p;
    t->selfclose = !t->closing && lastc == '/';
    *pp = p + 1;
    return 0;
}

/* Find attribute `name` in a tag; returns 0 with value span, else -1. */
static int attr_get(const struct tag *t, const char *name, const uint8_t **vp, int *vlen)
{
    const uint8_t *p = t->at, *end = t->aend;
    while (p < end) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end || *p == '/' || *p == '>') break;
        const uint8_t *ns = p;
        while (p < end && !is_ws(*p) && *p != '=' && *p != '/' && *p != '>') p++;
        int nl = p - ns;
        if (nl == 0) { p++; continue; }             /* defensive: always advance */
        while (p < end && is_ws(*p)) p++;
        if (p >= end || *p != '=') continue;        /* valueless attr: ignore */
        p++;
        while (p < end && is_ws(*p)) p++;
        if (p >= end || (*p != '"' && *p != '\'')) continue;
        int qc = *p++;
        const uint8_t *vs = p;
        while (p < end && *p != qc) p++;
        if (str_eq(ns, nl, name)) { *vp = vs; *vlen = p - vs; return 0; }
        if (p < end) p++;
    }
    return -1;
}

static int tag_is(const struct tag *t, const char *s) { return str_eq(t->name, t->nlen, s); }

/* A numeric attribute, straight into a 24.8 fixed value. */
static int attr_num(const struct tag *t, const char *name, int *out)
{
    const uint8_t *v; int vl;
    if (attr_get(t, name, &v, &vl)) return 0;
    const uint8_t *q = v;
    return pnum_fx(&q, v + vl, out) ? 0 : 1;
}

/* width/height: a bare number or "Npx"; anything else (%, em, ...) counts as
 * absent. Returns 0 on success. */
static int attr_dim_fx(const struct tag *t, const char *name, int *out)
{
    const uint8_t *v; int vl;
    if (attr_get(t, name, &v, &vl)) return -1;
    const uint8_t *p = v, *end = v + vl;
    while (p < end && is_ws(*p)) p++;
    if (pnum_fx(&p, end, out)) return -1;
    while (p < end && is_ws(*p)) p++;
    if (p == end) return 0;
    if (end - p == 2 && p[0] == 'p' && p[1] == 'x') return 0;
    return -1;
}

/* Skip the body of an unknown element (defs/style/title/text/...): count
 * open/close tags until the matching close. Never trusts the markup. */
static void skip_subtree(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp;
    int depth = 1;
    while (p < end && depth > 0) {
        while (p < end && *p != '<') p++;
        if (p >= end) break;
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {   /* comment */
            p += 4;
            while (p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++;
            p = p + 2 < end ? p + 3 : end;
            continue;
        }
        if (p + 1 < end && p[1] == '?') {
            p += 2;
            while (p + 1 < end && !(p[0] == '?' && p[1] == '>')) p++;
            p = p + 1 < end ? p + 2 : end;
            continue;
        }
        if (p + 1 < end && p[1] == '!') {                                  /* doctype-ish */
            while (p < end && *p != '>') p++;
            if (p < end) p++;
            continue;
        }
        struct tag t;
        if (parse_tag(&p, end, &t)) break;
        if (t.closing) depth--;
        else if (!t.selfclose) depth++;
    }
    *pp = p;
}

/* ---- paint + style state ---- */
struct paint { uint8_t r, g, b, a; int none, evenodd; };

/* Inherited element state: fill AND stroke, plus stroke-width (24.8, USER
 * units -- scaled to device by DXY() at the point a shape is actually
 * painted, same as every other length in this file). SVG's stroke default
 * is none, so stroke.none starts at 1 unlike fill.none. */
struct style { struct paint fill, stroke; int sw; };

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const struct { const char *n; uint8_t r, g, b; } CNAMES[] = {
    { "black", 0, 0, 0 },       { "white", 255, 255, 255 },
    { "red", 255, 0, 0 },       { "green", 0, 128, 0 },
    { "blue", 0, 0, 255 },      { "yellow", 255, 255, 0 },
    { "gray", 128, 128, 128 },  { "grey", 128, 128, 128 },
    { "silver", 192, 192, 192 },{ "maroon", 128, 0, 0 },
    { "purple", 128, 0, 128 },  { "fuchsia", 255, 0, 255 },
    { "lime", 0, 255, 0 },      { "olive", 128, 128, 0 },
    { "navy", 0, 0, 128 },      { "teal", 0, 128, 128 },
    { "aqua", 0, 255, 255 },    { "orange", 255, 165, 0 },
    { "rebeccapurple", 102, 51, 153 },
};

static uint8_t clampbyte(long long v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static void parse_color(const uint8_t *v, int vl, struct paint *pc)
{
    while (vl > 0 && is_ws(*v)) { v++; vl--; }
    while (vl > 0 && is_ws(v[vl - 1])) vl--;
    if (vl <= 0) return;
    if (str_eq(v, vl, "none")) { pc->none = 1; return; }
    if (str_eq(v, vl, "currentColor")) { pc->r = pc->g = pc->b = 0; pc->none = 0; return; }
    if (str_eq(v, vl, "transparent")) { pc->a = 0; pc->none = 0; return; }
    if (v[0] == '#') {
        int h[8], nd = 0;
        for (int i = 1; i < vl && nd < 8; i++) {
            int x = hexval(v[i]);
            if (x < 0) return;
            h[nd++] = x;
        }
        pc->none = 0;
        if (nd == 3 || nd == 4) {
            pc->r = h[0] * 17; pc->g = h[1] * 17; pc->b = h[2] * 17;
            if (nd == 4) pc->a = h[3] * 17;
        } else if (nd == 6 || nd == 8) {
            pc->r = h[0] * 16 + h[1]; pc->g = h[2] * 16 + h[3]; pc->b = h[4] * 16 + h[5];
            if (nd == 8) pc->a = h[6] * 16 + h[7];
        }
        return;
    }
    if (vl >= 4 && v[0] == 'r' && v[1] == 'g' && v[2] == 'b' && (v[3] == '(' || (vl >= 5 && v[3] == 'a' && v[4] == '('))) {
        const uint8_t *p = v + (v[3] == '(' ? 4 : 5), *end = v + vl;
        int c[4] = { 0, 0, 0, 256 };                 /* 24.8; default alpha 1.0 */
        for (int i = 0; i < 4; i++) {
            while (p < end && (is_ws(*p) || *p == ',')) p++;
            if (p >= end || *p == ')') break;
            int cv;
            if (pnum_fx(&p, end, &cv)) break;
            c[i] = cv;
            if (p < end && *p == '%') { c[i] = (int)imuldiv(c[i], 255, 100); p++; }
        }
        pc->none = 0;
        pc->r = clampbyte(c[0] >> 8);
        pc->g = clampbyte(c[1] >> 8);
        pc->b = clampbyte(c[2] >> 8);
        int a = c[3];
        if (a < 0) a = 0; if (a > 256) a = 256;       /* clamp to [0,1] in 24.8 */
        pc->a = (uint8_t)((a * 255) / 256);
        return;
    }
    for (unsigned i = 0; i < sizeof CNAMES / sizeof CNAMES[0]; i++)
        if (str_eq(v, vl, CNAMES[i].n)) {
            pc->r = CNAMES[i].r; pc->g = CNAMES[i].g; pc->b = CNAMES[i].b;
            pc->none = 0;
            return;
        }
    /* unknown color: keep inherited */
}

static void apply_opacity(const uint8_t *v, int vl, struct paint *pc)
{
    const uint8_t *p = v;
    while (p < v + vl && is_ws(*p)) p++;
    int f;
    if (pnum_fx(&p, v + vl, &f)) return;
    if (f < 0) f = 0; if (f > 256) f = 256;
    pc->a = (uint8_t)(((int)pc->a * f) >> 8);
}

/* Overlay an element's fill/stroke/opacity attributes onto inherited state. */
static void apply_style(const struct tag *t, struct style *st)
{
    const uint8_t *v; int vl;
    if (!attr_get(t, "fill", &v, &vl)) parse_color(v, vl, &st->fill);
    if (!attr_get(t, "fill-opacity", &v, &vl)) apply_opacity(v, vl, &st->fill);
    if (!attr_get(t, "fill-rule", &v, &vl)) {
        if (str_eq(v, vl, "evenodd")) st->fill.evenodd = 1;
        else if (str_eq(v, vl, "nonzero")) st->fill.evenodd = 0;
    }
    if (!attr_get(t, "stroke", &v, &vl)) parse_color(v, vl, &st->stroke);
    if (!attr_get(t, "stroke-opacity", &v, &vl)) apply_opacity(v, vl, &st->stroke);
    int sw;
    if (attr_num(t, "stroke-width", &sw) && sw > 0) st->sw = sw;
    if (!attr_get(t, "opacity", &v, &vl)) { apply_opacity(v, vl, &st->fill); apply_opacity(v, vl, &st->stroke); }
}

/* ---- viewBox transform: user coords -> device 24.8, uniform scale + translate --
 * s is 16.16, ox/oy are 24.8 -- deliberately not folded into a gfx_matrix and
 * applied via the path's CTM: the arc-to-cubic conversion (arc_to() below)
 * needs rx/ry and the current point in DEVICE space to do its own trig, so
 * every coordinate in this file is pre-scaled by RX/RY/DXY before it ever
 * reaches a gfx_* path builder, and every path is built under gfx_path's
 * default IDENTITY matrix. */
struct vbxform { long long s; int ox, oy; };

#define RX(vb, u)  ((int)(imul_shr16((long long)(u), (vb)->s) + (vb)->ox))
#define RY(vb, u)  ((int)(imul_shr16((long long)(u), (vb)->s) + (vb)->oy))
#define DXY(vb, u) ((int)imul_shr16((long long)(u), (vb)->s))

/* ---- elliptical arc (SVG spec F.6.5, endpoint parameterization) ----
 * Converts to <= 90-degree cubic segments and appends them to `gp` via
 * gfx_cubic_to -- gp->cx/gp->cy (already device 24.8) are the arc's start
 * point, rx/ry/x1/y1 arrive already device-scaled by the caller (mirroring
 * how the pre-Open-Logit version worked: this function has always operated
 * purely in device space, only the arithmetic underneath changed). */
static void arc_to(struct gfx_path *gp, int rx, int ry, int phi_deg256,
                   int large, int sweep, int x1, int y1)
{
    int x0 = gp->cx, y0 = gp->cy;
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (rx < 1 || ry < 1) { gfx_line_to(gp, x1, y1); return; }   /* < 1/256 device px: treat as a line */
    if (x0 == x1 && y0 == y1) return;

    long long cp = gfx_cos(phi_deg256), sp = gfx_sin(phi_deg256);
    long long dx = (x0 - x1) / 2, dy = (y0 - y1) / 2;
    long long xp = imul_shr16(dx, cp) + imul_shr16(dy, sp);
    long long yp = imul_shr16(dy, cp) - imul_shr16(dx, sp);

    __int128 rx2 = (__int128)rx * rx, ry2 = (__int128)ry * ry;
    __int128 xp2 = (__int128)xp * xp, yp2 = (__int128)yp * yp;
    __int128 lamnum = ry2 * xp2 + rx2 * yp2, lamden = rx2 * ry2;
    if (lamden > 0 && lamnum > lamden) {
        unsigned long long sq16 = sqrt_ratio16((lamnum * 65536) / lamden);
        rx = (int)imul_shr16(rx, (long long)sq16);
        ry = (int)imul_shr16(ry, (long long)sq16);
        rx2 = (__int128)rx * rx; ry2 = (__int128)ry * ry;
    }

    __int128 num = rx2 * ry2 - rx2 * yp2 - ry2 * xp2;
    __int128 den = rx2 * yp2 + ry2 * xp2;
    long long co16 = 0;
    if (den > 0) {
        __int128 n = num > 0 ? num : 0;
        co16 = (long long)sqrt_ratio16((n * 65536) / den);
    }
    if (large == sweep) co16 = -co16;

    long long cxp = imuldiv(imul_shr16(rx, co16), yp, ry);
    long long cyp = -imuldiv(imul_shr16(ry, co16), xp, rx);
    long long ccx = imul_shr16(cxp, cp) - imul_shr16(cyp, sp) + (x0 + x1) / 2;
    long long ccy = imul_shr16(cxp, sp) + imul_shr16(cyp, cp) + (y0 + y1) / 2;

    long long ux = imuldiv(xp - cxp, 65536, rx), uy = imuldiv(yp - cyp, 65536, ry);
    long long vx = imuldiv(-xp - cxp, 65536, rx), vy = imuldiv(-yp - cyp, 65536, ry);

    int th1 = iatan2_deg256(uy, ux);
    long long cross = imul_shr16(ux, vy) - imul_shr16(uy, vx);
    long long dot = imul_shr16(ux, vx) + imul_shr16(uy, vy);
    int dth = iatan2_deg256(cross, dot);
    if (!sweep && dth > 0) dth -= 360 * 256;
    if (sweep && dth < 0) dth += 360 * 256;

    int adth = dth < 0 ? -dth : dth;
    int nseg = adth / (90 * 256) + 1;
    if (nseg > 64) nseg = 64;
    int step = dth / nseg;

    for (int i = 0; i < nseg; i++) {
        int a1 = th1 + i * step, a2 = a1 + step;
        long long k = itan_deg256(step / 4) * 4 / 3;
        long long c1 = gfx_cos(a1), s1 = gfx_sin(a1), c2 = gfx_cos(a2), s2 = gfx_sin(a2);
        long long rxc1 = imul_shr16(rx, c1), rxs1 = imul_shr16(rx, s1);
        long long ryc1 = imul_shr16(ry, c1), rys1 = imul_shr16(ry, s1);
        long long rxc2 = imul_shr16(rx, c2), rxs2 = imul_shr16(rx, s2);
        long long ryc2 = imul_shr16(ry, c2), rys2 = imul_shr16(ry, s2);

        long long ex2 = ccx + imul_shr16(rxc2, cp) - imul_shr16(rys2, sp);
        long long ey2 = ccy + imul_shr16(rxc2, sp) + imul_shr16(rys2, cp);
        long long dx1 = -imul_shr16(rxs1, cp) - imul_shr16(ryc1, sp);
        long long dy1 = -imul_shr16(rxs1, sp) + imul_shr16(ryc1, cp);
        long long dx2 = -imul_shr16(rxs2, cp) - imul_shr16(ryc2, sp);
        long long dy2 = -imul_shr16(rxs2, sp) + imul_shr16(ryc2, cp);
        long long ex1 = ccx + imul_shr16(rxc1, cp) - imul_shr16(rys1, sp);
        long long ey1 = ccy + imul_shr16(rxc1, sp) + imul_shr16(rys1, cp);

        gfx_cubic_to(gp,
                     (int)clampfx(ex1 + imul_shr16(dx1, k)), (int)clampfx(ey1 + imul_shr16(dy1, k)),
                     (int)clampfx(ex2 - imul_shr16(dx2, k)), (int)clampfx(ey2 - imul_shr16(dy2, k)),
                     (int)clampfx(ex2), (int)clampfx(ey2));
    }
}

/* ---- path parsing: builds a gfx_path directly, in device coords ---- */
struct pp {
    const uint8_t *p, *end;
    struct gfx_path *gp;
    const struct vbxform *vb;
    int pcx, pcy;                                  /* last cubic ctrl, for S */
    int pqx, pqy;                                  /* last quad ctrl, for T */
};

static int pargs(struct pp *P, int *v, int cnt)
{
    for (int i = 0; i < cnt; i++) {
        while (P->p < P->end && (is_ws(*P->p) || *P->p == ',')) P->p++;
        if (pnum_fx(&P->p, P->end, &v[i])) return -1;
    }
    return 0;
}

/* Parse path data `d` and append its geometry to `gp` (already reset by the
 * caller). Stops (keeping what it has) at the first malformed or
 * unsupported command -- fill and stroke both still run on whatever was
 * recorded, same "never crash, never draw more than was there" rule as
 * before.
 *
 * Closure and Z, on purpose: an explicit Z appends the missing edge back to
 * the subpath's start (so the subpath's last recorded point equals its
 * first) and calls gfx_close(). A subpath that ends WITHOUT Z -- a new M, or
 * simply running out of `d` -- gets neither: gfx_raster.c closes every
 * subpath implicitly for FILL regardless (see gfx_close()'s comment in
 * gfx_path.c), but gfx_stroke_path can only tell a subpath is closed by
 * that same last-point-equals-first evidence, and per the SVG spec an
 * un-Z'd subpath is OPEN for stroking (two caps, not a join). Forcing the
 * duplicate point unconditionally -- which is what the old fill-only
 * scanline algorithm effectively needed -- would silently join every
 * un-closed subpath's stroke ends. */
static void parse_path(struct gfx_path *gp, const struct vbxform *vb, const uint8_t *d, int dlen)
{
    struct pp P;
    P.p = d; P.end = d + dlen; P.gp = gp; P.vb = vb;
    P.pcx = P.pcy = P.pqx = P.pqy = 0;
    int cmd = 0, last = 0;
    int v[7];
    for (;;) {
        while (P.p < P.end && (is_ws(*P.p) || *P.p == ',')) P.p++;
        if (P.p >= P.end) break;
        int c = *P.p;
        if (is_alpha(c)) { cmd = c; P.p++; }
        else if (!cmd) break;
        if (cmd == 'z' || cmd == 'Z') {
            if (gp->cx != gp->sx || gp->cy != gp->sy) gfx_line_to(gp, gp->sx, gp->sy);
            gfx_close(gp);
            cmd = 0; last = 'z';
            continue;
        }
        int rel = cmd >= 'a' && cmd <= 'z';
        int eff = rel ? cmd - 32 : cmd;
        if (eff == 'M') eff = 'L';                      /* extra pairs are implicit lines */
        switch (eff) {
        case 'M': case 'L': {
            if (pargs(&P, v, 2)) goto done;
            int nx = rel ? gp->cx + DXY(vb, v[0]) : RX(vb, v[0]);
            int ny = rel ? gp->cy + DXY(vb, v[1]) : RY(vb, v[1]);
            if (cmd == 'M' || cmd == 'm') {
                gfx_move_to(gp, nx, ny);
                cmd = rel ? 'l' : 'L';                  /* further pairs are linetos */
            } else {
                gfx_line_to(gp, nx, ny);
            }
            last = 'L';
            break;
        }
        case 'H': {
            if (pargs(&P, v, 1)) goto done;
            int nx = rel ? gp->cx + DXY(vb, v[0]) : RX(vb, v[0]);
            gfx_line_to(gp, nx, gp->cy);
            last = 'L';
            break;
        }
        case 'V': {
            if (pargs(&P, v, 1)) goto done;
            int ny = rel ? gp->cy + DXY(vb, v[0]) : RY(vb, v[0]);
            gfx_line_to(gp, gp->cx, ny);
            last = 'L';
            break;
        }
        case 'C': case 'S': {
            if (pargs(&P, v, eff == 'C' ? 6 : 4)) goto done;
            int x1, y1, x2, y2, x3, y3;
            int scx = gp->cx, scy = gp->cy;
            if (eff == 'C') {
                x1 = rel ? scx + DXY(vb, v[0]) : RX(vb, v[0]);
                y1 = rel ? scy + DXY(vb, v[1]) : RY(vb, v[1]);
                x2 = rel ? scx + DXY(vb, v[2]) : RX(vb, v[2]);
                y2 = rel ? scy + DXY(vb, v[3]) : RY(vb, v[3]);
                x3 = rel ? scx + DXY(vb, v[4]) : RX(vb, v[4]);
                y3 = rel ? scy + DXY(vb, v[5]) : RY(vb, v[5]);
            } else {
                x1 = last == 'C' ? 2 * scx - P.pcx : scx;
                y1 = last == 'C' ? 2 * scy - P.pcy : scy;
                x2 = rel ? scx + DXY(vb, v[0]) : RX(vb, v[0]);
                y2 = rel ? scy + DXY(vb, v[1]) : RY(vb, v[1]);
                x3 = rel ? scx + DXY(vb, v[2]) : RX(vb, v[2]);
                y3 = rel ? scy + DXY(vb, v[3]) : RY(vb, v[3]);
            }
            P.pcx = x2; P.pcy = y2;
            gfx_cubic_to(gp, x1, y1, x2, y2, x3, y3);
            last = 'C';
            break;
        }
        case 'Q': case 'T': {
            if (pargs(&P, v, eff == 'Q' ? 4 : 2)) goto done;
            int qx, qy, x3, y3;
            int scx = gp->cx, scy = gp->cy;
            if (eff == 'Q') {
                qx = rel ? scx + DXY(vb, v[0]) : RX(vb, v[0]);
                qy = rel ? scy + DXY(vb, v[1]) : RY(vb, v[1]);
                x3 = rel ? scx + DXY(vb, v[2]) : RX(vb, v[2]);
                y3 = rel ? scy + DXY(vb, v[3]) : RY(vb, v[3]);
            } else {
                qx = last == 'Q' ? 2 * scx - P.pqx : scx;
                qy = last == 'Q' ? 2 * scy - P.pqy : scy;
                x3 = rel ? scx + DXY(vb, v[0]) : RX(vb, v[0]);
                y3 = rel ? scy + DXY(vb, v[1]) : RY(vb, v[1]);
            }
            P.pqx = qx; P.pqy = qy;
            gfx_quad_to(gp, qx, qy, x3, y3);
            last = 'Q';
            break;
        }
        case 'A': {
            if (pargs(&P, v, 7)) goto done;
            int x3 = rel ? gp->cx + DXY(vb, v[5]) : RX(vb, v[5]);
            int y3 = rel ? gp->cy + DXY(vb, v[6]) : RY(vb, v[6]);
            arc_to(gp, DXY(vb, v[0]), DXY(vb, v[1]), v[2], v[3] != 0, v[4] != 0, x3, y3);
            last = 'A';
            break;
        }
        default:
            goto done;                                  /* unsupported command: keep what we have */
        }
    }
done:
    return;
}

/* ---- simple shapes: engine sugar, plus the rect closure note above ---- */
static void build_rect(struct gfx_path *gp, const struct vbxform *vb, const struct tag *t)
{
    int x = 0, y = 0, w = 0, h = 0;
    attr_num(t, "x", &x); attr_num(t, "y", &y);
    attr_num(t, "width", &w); attr_num(t, "height", &h);
    if (w <= 0 || h <= 0) return;
    int x0 = RX(vb, x), y0 = RY(vb, y), x1 = RX(vb, x + w), y1 = RY(vb, y + h);
    gfx_move_to(gp, x0, y0);
    gfx_line_to(gp, x1, y0);
    gfx_line_to(gp, x1, y1);
    gfx_line_to(gp, x0, y1);
    gfx_line_to(gp, x0, y0);      /* explicit closing edge -- see file comment on gfx_path_rect */
    gfx_close(gp);
}

static void build_ellipse(struct gfx_path *gp, const struct vbxform *vb, const struct tag *t, int ellipse)
{
    int cx = 0, cy = 0, rx = 0, ry = 0;
    attr_num(t, "cx", &cx); attr_num(t, "cy", &cy);
    if (ellipse) { attr_num(t, "rx", &rx); attr_num(t, "ry", &ry); }
    else { attr_num(t, "r", &rx); ry = rx; }
    if (rx <= 0 || ry <= 0) return;
    gfx_path_ellipse(gp, RX(vb, cx), RY(vb, cy), DXY(vb, rx), DXY(vb, ry));
}

/* ---- fill + stroke one already-built path -----------------------------
 * Path storage is static and reused per element (gfx_path_reset between
 * shapes) rather than kmalloc'd per shape: this file compiles into three
 * ring-3 binaries that decode one image at a time, never concurrently, so
 * there is nothing to protect by allocating fresh buffers every call and a
 * kmalloc/kfree pair per icon shape is pure overhead. GFX_MAX_EDGES (8192,
 * gfx.h) is documented there as "generous for SVG path data: a hand-drawn
 * icon runs to dozens of segments, not thousands" -- SVG_PTCAP matches it
 * for the same reason raster.c's own caps were reused rather than guessed.
 * A path that genuinely overflows is refused by gfx_fill/gfx_stroke_path
 * (they check ->overflow and draw nothing) rather than drawn truncated. */
#define SVG_PTCAP   8192
#define SVG_SUBCAP  512
#define SVG_SPTCAP  16896     /* ~2x SVG_PTCAP + slack, per gfx_stroke_path's own sizing note */
#define SVG_SSUBCAP 1056

static int g_pt[SVG_PTCAP * 2];
static int g_sub[SVG_SUBCAP];
static int g_spt[SVG_SPTCAP * 2];
static int g_ssub[SVG_SSUBCAP];
static struct gfx_path g_spath;

static void paint_shape(struct gfx_surface *surf, struct gfx_path *gp,
                        const struct style *st, const struct vbxform *vb)
{
    if (!st->fill.none && st->fill.a) {
        struct gfx_paint p;
        gfx_paint_solid(&p, GFX_RGB(st->fill.r, st->fill.g, st->fill.b), st->fill.a);
        gfx_fill(surf, gp, st->fill.evenodd ? GFX_EVENODD : GFX_NONZERO, &p, 0);
    }
    if (!st->stroke.none && st->stroke.a && st->sw > 0) {
        int devw = DXY(vb, st->sw);
        if (devw > 0) {
            struct gfx_stroke sd;
            sd.width = devw;
            sd.cap = GFX_CAP_BUTT;
            sd.join = GFX_JOIN_MITER;
            sd.miter_limit = 4 << 16;                  /* SVG default miter-limit */
            sd.dash = 0; sd.ndash = 0; sd.dash_phase = 0;
            gfx_path_reset(&g_spath);
            if (gfx_stroke_path(&g_spath, gp, &sd)) {
                struct gfx_paint sp;
                gfx_paint_solid(&sp, GFX_RGB(st->stroke.r, st->stroke.g, st->stroke.b), st->stroke.a);
                gfx_fill(surf, &g_spath, GFX_NONZERO, &sp, 0);
            }
        }
    }
}

/* ---- content sniffing (unchanged: pure byte-level, no coordinates) ---- */
/* Everything before the root <svg must be whitespace, <?xml?>, comments or
 * a <!DOCTYPE> -- this rejects HTML documents that merely contain an svg. */
static int prefix_ok(const uint8_t *p, int lim)
{
    int i = 0;
    if (lim >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;  /* UTF-8 BOM */
    while (i < lim) {
        int c = p[i];
        if (is_ws(c)) { i++; continue; }
        if (c == '<' && i + 1 < lim && p[i + 1] == '?') {
            i += 2;
            while (i + 1 < lim && !(p[i] == '?' && p[i + 1] == '>')) i++;
            if (i + 1 >= lim) return 0;
            i += 2;
            continue;
        }
        if (c == '<' && i + 3 < lim && p[i + 1] == '!' && p[i + 2] == '-' && p[i + 3] == '-') {
            i += 4;
            while (i + 2 < lim && !(p[i] == '-' && p[i + 1] == '-' && p[i + 2] == '>')) i++;
            if (i + 2 >= lim) return 0;
            i += 3;
            continue;
        }
        if (c == '<' && i + 1 < lim && p[i + 1] == '!') {
            while (i < lim && p[i] != '>') i++;
            if (i >= lim) return 0;
            i++;
            continue;
        }
        return 0;
    }
    return 1;
}

static int svg_detect(const uint8_t *p, int n)
{
    int lim = n < 1024 ? n : 1024;
    for (int i = 0; i + 4 <= lim; i++)
        if (p[i] == '<' && p[i + 1] == 's' && p[i + 2] == 'v' && p[i + 3] == 'g') {
            if (i + 4 >= n) return prefix_ok(p, i);
            int c = p[i + 4];
            if (is_ws(c) || c == '>' || c == '/')
                return prefix_ok(p, i);
        }
    return 0;
}

/* ---- decoder ---- */
static int svg_decode(const uint8_t *p, int n, struct image *out)
{
    if (!p || n <= 4) return -1;
    const uint8_t *cur = p, *end = p + n;

    /* locate the root <svg> and its viewport */
    struct tag root;
    int found = 0;
    while (cur < end) {
        while (cur < end && *cur != '<') cur++;
        if (cur >= end) break;
        if (cur + 3 < end && cur[1] == '!' && cur[2] == '-' && cur[3] == '-') {
            cur += 4;
            while (cur + 2 < end && !(cur[0] == '-' && cur[1] == '-' && cur[2] == '>')) cur++;
            cur = cur + 2 < end ? cur + 3 : end;
            continue;
        }
        if (cur + 1 < end && (cur[1] == '?' || cur[1] == '!')) {
            while (cur < end && *cur != '>') cur++;
            if (cur < end) cur++;
            continue;
        }
        if (cur + 1 < end && cur[1] == '/') {               /* stray close tag */
            while (cur < end && *cur != '>') cur++;
            if (cur < end) cur++;
            continue;
        }
        if (parse_tag(&cur, end, &root)) break;
        if (tag_is(&root, "svg")) { found = 1; break; }
    }
    if (!found) return -1;

    int dw = 0, dh = 0, vbx = 0, vby = 0, vbw = 0, vbh = 0;
    int has_w = !attr_dim_fx(&root, "width", &dw);
    int has_h = !attr_dim_fx(&root, "height", &dh);
    int has_vb = 0;
    const uint8_t *v; int vl;
    if (!attr_get(&root, "viewBox", &v, &vl)) {
        const uint8_t *q = v, *qe = v + vl;
        if (!pnum_fx(&q, qe, &vbx)) {
            while (q < qe && (is_ws(*q) || *q == ',')) q++;
            if (!pnum_fx(&q, qe, &vby)) {
                while (q < qe && (is_ws(*q) || *q == ',')) q++;
                if (!pnum_fx(&q, qe, &vbw)) {
                    while (q < qe && (is_ws(*q) || *q == ',')) q++;
                    if (!pnum_fx(&q, qe, &vbh) && vbw > 0 && vbh > 0) has_vb = 1;
                }
            }
        }
    }
    if (!has_w && has_vb) { dw = vbw; has_w = 1; }
    if (!has_h && has_vb) { dh = vbh; has_h = 1; }
    if (!has_w && has_h && has_vb) { dw = (int)imuldiv(dh, vbw, vbh); has_w = 1; }
    if (!has_h && has_w && has_vb) { dh = (int)imuldiv(dw, vbh, vbw); has_h = 1; }
    if (!has_w) dw = 300 * 256;
    if (!has_h) dh = 150 * 256;
    int w = (dw + 128) >> 8, h = (dh + 128) >> 8;
    if (w < 1) w = 1; if (w > 2048) w = 2048;
    if (h < 1) h = 1; if (h > 2048) h = 2048;
    while ((long)w * h > 4L * 1024 * 1024) { if (w >= h) w = (w + 1) / 2; else h = (h + 1) / 2; }

    uint8_t *px = kmalloc((unsigned long)w * h * 4);
    if (!px) return -1;

    struct gfx_surface surf;
    gfx_surface_init(&surf, px, w, h, 0);
    gfx_surface_clear(&surf);

    struct vbxform vb;
    if (has_vb) {
        long long sx16 = imuldiv((long long)w << 8, 65536, vbw);
        long long sy16 = imuldiv((long long)h << 8, 65536, vbh);
        vb.s = sx16 < sy16 ? sx16 : sy16;                   /* xMidYMid meet */
        vb.ox = (int)((((long long)w << 8) - imul_shr16(vbw, vb.s)) / 2 - imul_shr16(vbx, vb.s));
        vb.oy = (int)((((long long)h << 8) - imul_shr16(vbh, vb.s)) / 2 - imul_shr16(vby, vb.s));
    } else {
        vb.s = 65536; vb.ox = 0; vb.oy = 0;
    }

    struct gfx_path path;
    gfx_path_init(&path, g_pt, SVG_PTCAP, g_sub, SVG_SUBCAP);
    gfx_path_init(&g_spath, g_spt, SVG_SPTCAP, g_ssub, SVG_SSUBCAP);

    /* render walk over the root's content */
    struct style stk[32];
    int sp = 0;
    stk[0].fill.r = stk[0].fill.g = stk[0].fill.b = 0; stk[0].fill.a = 255;
    stk[0].fill.none = 0; stk[0].fill.evenodd = 0;
    stk[0].stroke.r = stk[0].stroke.g = stk[0].stroke.b = 0; stk[0].stroke.a = 255;
    stk[0].stroke.none = 1; stk[0].stroke.evenodd = 0;
    stk[0].sw = 256;                                        /* SVG default stroke-width: 1 */
    apply_style(&root, &stk[0]);

    while (cur < end) {
        while (cur < end && *cur != '<') cur++;
        if (cur >= end) break;
        if (cur + 3 < end && cur[1] == '!' && cur[2] == '-' && cur[3] == '-') {
            cur += 4;
            while (cur + 2 < end && !(cur[0] == '-' && cur[1] == '-' && cur[2] == '>')) cur++;
            cur = cur + 2 < end ? cur + 3 : end;
            continue;
        }
        if (cur + 1 < end && (cur[1] == '?' || cur[1] == '!')) {
            while (cur < end && *cur != '>') cur++;
            if (cur < end) cur++;
            continue;
        }
        struct tag t;
        if (parse_tag(&cur, end, &t)) break;
        if (t.closing) {
            if ((tag_is(&t, "g") || tag_is(&t, "svg")) && sp > 0) sp--;
            continue;
        }
        if (tag_is(&t, "g") || tag_is(&t, "svg")) {
            if (sp < 31) {
                stk[sp + 1] = stk[sp];
                sp++;
                apply_style(&t, &stk[sp]);
            }
            if (t.selfclose && sp > 0) sp--;
            continue;
        }
        if (tag_is(&t, "path")) {
            struct style st = stk[sp];
            apply_style(&t, &st);
            if (!attr_get(&t, "d", &v, &vl)) {
                gfx_path_reset(&path);
                parse_path(&path, &vb, v, vl);
                paint_shape(&surf, &path, &st, &vb);
            }
            continue;
        }
        if (tag_is(&t, "rect")) {
            struct style st = stk[sp];
            apply_style(&t, &st);
            gfx_path_reset(&path);
            build_rect(&path, &vb, &t);
            paint_shape(&surf, &path, &st, &vb);
            continue;
        }
        if (tag_is(&t, "circle") || tag_is(&t, "ellipse")) {
            struct style st = stk[sp];
            apply_style(&t, &st);
            gfx_path_reset(&path);
            build_ellipse(&path, &vb, &t, tag_is(&t, "ellipse"));
            paint_shape(&surf, &path, &st, &vb);
            continue;
        }
        if (!t.selfclose) skip_subtree(&cur, end);          /* defs/style/text/... */
    }

    out->w = w; out->h = h; out->rgba = px;
    return 0;
}

void svg_register(void) { img_register(svg_detect, svg_decode); }
