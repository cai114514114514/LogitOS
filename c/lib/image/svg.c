/* Minimal SVG rasterizer, aimed at web icons (GitHub octicons are the
 * reference corpus). Supported subset:
 *   <svg width/height/viewBox>, <g>/<svg> nesting with inherited fill,
 *   <path d> commands M L H V C S Q T A Z (absolute + relative),
 *   <rect>, <circle>, <ellipse>;
 *   fill = #rgb/#rrggbb/#rgba/#rrggbbaa/rgb()/rgba()/keyword/currentColor
 *   (-> black)/none, fill-opacity, opacity, fill-rule=evenodd (nonzero
 *   default). Anything unsupported (transform, style, defs, text, ...) is
 *   skipped, never fatal; malformed input must never crash or overrun.
 * Rendering: curves are flattened to edges (adaptive cubic subdivision in
 * device space) and filled scanline-by-scanline with a sorted-crossing
 * winding walk -- icons are tiny, so this is plenty fast. */
#include "img.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memcpy(void *, const void *, unsigned long);

/* ---- tiny freestanding double math (no libm in the kernel) ---- */
#define DPI 3.14159265358979323846

static double dabs(double v) { return v < 0 ? -v : v; }

static double dsqrt(double v)
{
    if (v <= 0) return 0;
    double g = v >= 1 ? v : 1;
    for (int i = 0; i < 80; i++) {
        double ng = 0.5 * (g + v / g);
        if (ng == g) break;
        g = ng;
    }
    return g;
}

static double dsin(double t)
{
    const double P2 = DPI * 2;
    t -= (double)(long)(t / P2) * P2;           /* -> (-2pi, 2pi) for sane t */
    if (t > DPI) t -= P2;
    if (t < -DPI) t += P2;
    if (t > DPI / 2) t = DPI - t;
    else if (t < -DPI / 2) t = -DPI - t;        /* -> [-pi/2, pi/2] */
    double t2 = t * t;
    return t * (1 + t2 * (-1.0 / 6 + t2 * (1.0 / 120 + t2 * (-1.0 / 5040 +
               t2 * (1.0 / 362880 + t2 * (-1.0 / 39916800.0))))));
}

static double dcos(double t) { return dsin(t + DPI / 2); }
static double dtan(double t) { double c = dcos(t); return c != 0 ? dsin(t) / c : 0; }

/* atan for |t| <= 1: t*(pi/4 + 0.273*(1-|t|)), max err ~0.004 rad -- fine
 * for placing arc pixels on small icons. */
static double datan1(double t)
{
    double a = dabs(t);
    double r = a * (DPI / 4 + 0.273 * (1 - a));
    return t < 0 ? -r : r;
}

static double datan2(double y, double x)
{
    double ay = dabs(y), ax = dabs(x);
    if (ay == 0 && ax == 0) return 0;
    double r;
    if (ax >= ay) {
        r = datan1(y / x);
        if (x < 0) r += y >= 0 ? DPI : -DPI;
    } else {
        r = (y > 0 ? DPI / 2 : -DPI / 2) - datan1(x / y);
    }
    return r;
}

/* ceil for values pre-clamped to +-1e9 (safe (long) cast range) */
static long iceil(double v)
{
    if (v > 1e9) v = 1e9; else if (v < -1e9) v = -1e9;
    long t = (long)v;
    if ((double)t < v) t++;
    return t;
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

/* Parse a float; stops at the first char that cannot belong to the number
 * (so SVG's "1.2.3" reads as 1.2 then .3, "10-3" as 10 then -3). */
static int pnum(const uint8_t **pp, const uint8_t *end, double *out)
{
    const uint8_t *p = *pp;
    int sign = 1, any = 0, dot = 0;
    double v = 0, frac = 0.1;
    if (p < end && (*p == '-' || *p == '+')) { if (*p == '-') sign = -1; p++; }
    while (p < end) {
        int c = *p;
        if (is_digit(c)) {
            any = 1;
            if (!dot) { v = v * 10 + (c - '0'); if (v > 1e15) v = 1e15; }
            else { v += (c - '0') * frac; frac *= 0.1; }
            p++;
        } else if (c == '.' && !dot) { dot = 1; p++; }
        else break;
    }
    if (!any) return -1;
    if (p < end && (*p == 'e' || *p == 'E')) {                  /* exponent */
        const uint8_t *q = p + 1;
        int es = 1, e = 0, eany = 0;
        if (q < end && (*q == '-' || *q == '+')) { if (*q == '-') es = -1; q++; }
        while (q < end && is_digit(*q)) { eany = 1; e = e * 10 + (*q - '0'); if (e > 300) e = 300; q++; }
        if (eany) { p = q; while (e--) { if (es > 0) v *= 10; else v /= 10; if (v > 1e15) { v = 1e15; break; } } }
    }
    *out = sign * v;
    *pp = p;
    return 0;
}

/* ---- XML tag scanning ---- */
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

/* ---- paint state ---- */
struct paint { uint8_t r, g, b, a; int none, evenodd; };

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
        double c[4] = { 0, 0, 0, 1 };
        for (int i = 0; i < 4; i++) {
            while (p < end && (is_ws(*p) || *p == ',')) p++;
            if (p >= end || *p == ')') break;
            if (pnum(&p, end, &c[i])) break;
            if (p < end && *p == '%') { c[i] = c[i] * 255 / 100; p++; }
        }
        pc->none = 0;
        pc->r = (uint8_t)(c[0] < 0 ? 0 : c[0] > 255 ? 255 : c[0]);
        pc->g = (uint8_t)(c[1] < 0 ? 0 : c[1] > 255 ? 255 : c[1]);
        pc->b = (uint8_t)(c[2] < 0 ? 0 : c[2] > 255 ? 255 : c[2]);
        if (c[3] < 0) c[3] = 0; if (c[3] > 1) c[3] = 1;
        pc->a = (uint8_t)(255 * c[3]);
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
    double f;
    const uint8_t *p = v;
    while (p < v + vl && is_ws(*p)) p++;
    if (pnum(&p, v + vl, &f)) return;
    if (f < 0) f = 0; if (f > 1) f = 1;
    pc->a = (uint8_t)(pc->a * f);
}

/* Overlay an element's paint attributes onto the inherited state. */
static void apply_paint(const struct tag *t, struct paint *pc)
{
    const uint8_t *v; int vl;
    if (!attr_get(t, "fill", &v, &vl)) parse_color(v, vl, pc);
    if (!attr_get(t, "fill-opacity", &v, &vl)) apply_opacity(v, vl, pc);
    if (!attr_get(t, "opacity", &v, &vl)) apply_opacity(v, vl, pc);
    if (!attr_get(t, "fill-rule", &v, &vl)) {
        if (str_eq(v, vl, "evenodd")) pc->evenodd = 1;
        else if (str_eq(v, vl, "nonzero")) pc->evenodd = 0;
    }
}

/* ---- rasterizer ---- */
#define MAXEDGES 32768

struct rst {
    uint8_t *px; int w, h;
    double s, ox, oy;                               /* user -> device: d = u*s + o */
    double *edges; int ne, cape;                    /* 4 doubles per edge */
};

/* Clamp to sane device space: keeps double precision meaningful during
 * curve subdivision and bounds every downstream computation. */
static double dclamp(double v) { return v > 1e6 ? 1e6 : v < -1e6 ? -1e6 : v; }

static void edge_add(struct rst *R, double x0, double y0, double x1, double y1)
{
    if (y0 == y1) return;                           /* horizontals never cross a scanline */
    if (R->ne >= MAXEDGES) return;                  /* pathological input: partial render */
    if (R->ne == R->cape) {
        int nc = R->cape ? R->cape * 2 : 256;
        double *ne = kmalloc((unsigned long)nc * 4 * sizeof(double));
        if (!ne) return;
        if (R->edges) { memcpy(ne, R->edges, (unsigned long)R->ne * 4 * sizeof(double)); kfree(R->edges); }
        R->edges = ne; R->cape = nc;
    }
    double *e = R->edges + (long)R->ne * 4;
    e[0] = dclamp(x0); e[1] = dclamp(y0); e[2] = dclamp(x1); e[3] = dclamp(y1);
    R->ne++;
}

/* src-over blend one straight-alpha pixel */
static void put_px(uint8_t *px, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    unsigned da = px[3], sa = a;
    if (sa == 255 || da == 0) { px[0] = r; px[1] = g; px[2] = b; px[3] = sa ? sa : da; return; }
    if (!sa) return;
    unsigned oa = sa + (da * (255 - sa) + 127) / 255;
    px[0] = (uint8_t)((r * sa * 255 + px[0] * da * (255 - sa) + oa * 127) / (oa * 255));
    px[1] = (uint8_t)((g * sa * 255 + px[1] * da * (255 - sa) + oa * 127) / (oa * 255));
    px[2] = (uint8_t)((b * sa * 255 + px[2] * da * (255 - sa) + oa * 127) / (oa * 255));
    px[3] = (uint8_t)oa;
}

/* Scanline-fill the accumulated edge list, then reset it for the next shape. */
static void fill(struct rst *R, const struct paint *pc)
{
    int n = R->ne;
    if (!pc->none && pc->a && n > 0) {
        double *xs = kmalloc((unsigned long)n * sizeof(double));
        int *wd = kmalloc((unsigned long)n * sizeof(int));
        if (xs && wd) {
            for (int y = 0; y < R->h; y++) {
                double ys = y + 0.5;
                int nc = 0;
                for (int i = 0; i < n; i++) {
                    double *e = R->edges + (long)i * 4;
                    if ((e[1] <= ys) == (e[3] <= ys)) continue;
                    xs[nc] = e[0] + (ys - e[1]) * (e[2] - e[0]) / (e[3] - e[1]);
                    wd[nc] = e[3] > e[1] ? 1 : -1;
                    nc++;
                }
                /* shell sort by x: insertion sort is O(n^2) on adversarial
                 * inputs (thousands of vertical edges crossing one scanline) */
                for (int gap = nc / 2; gap > 0; gap /= 2)
                    for (int i = gap; i < nc; i++) {
                        double xv = xs[i]; int wv = wd[i], j = i - gap;
                        while (j >= 0 && xs[j] > xv) { xs[j + gap] = xs[j]; wd[j + gap] = wd[j]; j -= gap; }
                        xs[j + gap] = xv; wd[j + gap] = wv;
                    }
                int wind = 0;
                for (int i = 0; i + 1 < nc; i++) {
                    wind += wd[i];
                    int inside = pc->evenodd ? (wind & 1) : (wind != 0);
                    if (!inside) continue;
                    double xa = xs[i], xb = xs[i + 1];
                    if (xa > xb) { double t = xa; xa = xb; xb = t; }
                    if (xa < 0) xa = 0;
                    if (xb > R->w) xb = R->w;
                    uint8_t *row = R->px + (long)y * R->w * 4;
                    for (long x = iceil(xa - 0.5); x + 0.5 < xb; x++)
                        put_px(row + x * 4, pc->r, pc->g, pc->b, pc->a);
                }
            }
        }
        if (xs) kfree(xs);
        if (wd) kfree(wd);
    }
    R->ne = 0;
}

/* ---- path parsing (all geometry in device coords) ---- */
struct pp {
    const uint8_t *p, *end;
    struct rst *R;
    double cx, cy;                                  /* current point */
    double sx, sy;                                  /* subpath start */
    double pcx, pcy;                                /* last cubic ctrl (for S) */
    double pqx, pqy;                                /* last quad ctrl (for T) */
};

#define RX(R, u) ((u) * (R)->s + (R)->ox)
#define RY(R, u) ((u) * (R)->s + (R)->oy)
#define DXY(R, u) ((u) * (R)->s)

static void close_sub(struct pp *P)
{
    if (P->cx != P->sx || P->cy != P->sy)
        edge_add(P->R, P->cx, P->cy, P->sx, P->sy);
    P->cx = P->sx; P->cy = P->sy;
}

/* Flatten cubic from current point to (x3,y3) with ctrl (x1,y1),(x2,y2). */
static void cub4(struct pp *P, double x1, double y1, double x2, double y2,
                 double x3, double y3, int depth)
{
    double x0 = P->cx, y0 = P->cy;
    double dx = x3 - x0, dy = y3 - y0;
    double c1 = dabs((x1 - x3) * dy - (y1 - y3) * dx);      /* dist*len */
    double c2 = dabs((x2 - x3) * dy - (y2 - y3) * dx);
    double cc = c1 + c2;
    /* depth cap + zero-chord guard: with degenerate coords the flatness test
     * can never pass (midpoints stop moving), and without a cap each such
     * curve costs 2^depth recursions -- a hang vector on hostile input. */
    if (depth >= 12 || (dx == 0 && dy == 0) ||
        cc * cc <= 0.2 * (dx * dx + dy * dy) + 1e-12) {
        edge_add(P->R, x0, y0, x3, y3);
        return;
    }
    double x01 = (x0 + x1) / 2, y01 = (y0 + y1) / 2;
    double x12 = (x1 + x2) / 2, y12 = (y1 + y2) / 2;
    double x23 = (x2 + x3) / 2, y23 = (y2 + y3) / 2;
    double xa = (x01 + x12) / 2, ya = (y01 + y12) / 2;
    double xb = (x12 + x23) / 2, yb = (y12 + y23) / 2;
    double xm = (xa + xb) / 2, ym = (ya + yb) / 2;
    double sx = P->cx, sy = P->cy;
    cub4(P, x01, y01, xa, ya, xm, ym, depth + 1);
    P->cx = xm; P->cy = ym;
    cub4(P, xb, yb, x23, y23, x3, y3, depth + 1);
    P->cx = sx; P->cy = sy;                     /* caller restores endpoint */
}

static void quad(struct pp *P, double qx, double qy, double x3, double y3)
{
    double x0 = P->cx, y0 = P->cy;
    cub4(P, x0 + 2.0 / 3.0 * (qx - x0), y0 + 2.0 / 3.0 * (qy - y0),
            x3 + 2.0 / 3.0 * (qx - x3), y3 + 2.0 / 3.0 * (qy - y3), x3, y3, 0);
}

/* SVG elliptical arc (endpoint parameterization, spec F.6.5), converted to
 * cubics of <= 90 degrees each. All coordinates already device-space. */
static void arc_to(struct pp *P, double rx, double ry, double phi_deg,
                   int large, int sweep, double x1, double y1)
{
    double x0 = P->cx, y0 = P->cy;
    rx = dabs(rx); ry = dabs(ry);
    if (rx < 1e-9 || ry < 1e-9) { edge_add(P->R, x0, y0, x1, y1); return; }
    if (x0 == x1 && y0 == y1) return;
    double phi = phi_deg * (DPI / 180.0);
    double cp = dcos(phi), sp = dsin(phi);
    double dx = (x0 - x1) / 2, dy = (y0 - y1) / 2;
    double xp = cp * dx + sp * dy;
    double yp = -sp * dx + cp * dy;
    double lam = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lam > 1) { double sq = dsqrt(lam); rx *= sq; ry *= sq; }
    double num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
    double den = rx * rx * yp * yp + ry * ry * xp * xp;
    double co = den > 0 ? dsqrt(num > 0 ? num / den : 0) : 0;
    if (large == sweep) co = -co;
    double cxp = co * rx * yp / ry, cyp = -co * ry * xp / rx;
    double cx = cp * cxp - sp * cyp + (x0 + x1) / 2;
    double cy = sp * cxp + cp * cyp + (y0 + y1) / 2;
    double ux = (xp - cxp) / rx, uy = (yp - cyp) / ry;
    double vx = (-xp - cxp) / rx, vy = (-yp - cyp) / ry;
    double th1 = datan2(uy, ux);
    double dth = datan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && dth > 0) dth -= 2 * DPI;
    if (sweep && dth < 0) dth += 2 * DPI;
    int nseg = (int)(dabs(dth) / (DPI / 2)) + 1;
    if (nseg > 64) nseg = 64;
    double step = dth / nseg;
    for (int i = 0; i < nseg; i++) {
        double a1 = th1 + i * step, a2 = a1 + step;
        double k = 4.0 / 3.0 * dtan((a2 - a1) / 4);
        double c1 = dcos(a1), s1 = dsin(a1), c2 = dcos(a2), s2 = dsin(a2);
        double ex2 = cx + rx * c2 * cp - ry * s2 * sp;
        double ey2 = cy + rx * c2 * sp + ry * s2 * cp;
        double dx1 = -rx * s1 * cp - ry * c1 * sp, dy1 = -rx * s1 * sp + ry * c1 * cp;
        double dx2 = -rx * s2 * cp - ry * c2 * sp, dy2 = -rx * s2 * sp + ry * c2 * cp;
        double ex1 = cx + rx * c1 * cp - ry * s1 * sp;
        double ey1 = cy + rx * c1 * sp + ry * s1 * cp;
        cub4(P, ex1 + k * dx1, ey1 + k * dy1, ex2 - k * dx2, ey2 - k * dy2, ex2, ey2, 0);
        P->cx = ex2; P->cy = ey2;
    }
}

static int pargs(struct pp *P, double *v, int cnt)
{
    for (int i = 0; i < cnt; i++) {
        while (P->p < P->end && (is_ws(*P->p) || *P->p == ',')) P->p++;
        if (pnum(&P->p, P->end, &v[i])) return -1;
    }
    return 0;
}

/* Parse path data `d`, append edges, then fill with `pc`. Stops (keeping
 * what it has) at the first malformed or unsupported command. */
static void parse_path(struct rst *R, const uint8_t *d, int dlen, const struct paint *pc)
{
    struct pp P;
    P.p = d; P.end = d + dlen; P.R = R;
    P.cx = P.cy = P.sx = P.sy = 0;
    P.pcx = P.pcy = P.pqx = P.pqy = 0;
    int cmd = 0, last = 0, have = 0;
    double v[7];
    for (;;) {
        while (P.p < P.end && (is_ws(*P.p) || *P.p == ',')) P.p++;
        if (P.p >= P.end) break;
        int c = *P.p;
        if (is_alpha(c)) { cmd = c; P.p++; }
        else if (!cmd) break;
        if (cmd == 'z' || cmd == 'Z') {
            if (have) close_sub(&P);
            cmd = 0; last = 'z';
            continue;
        }
        int rel = cmd >= 'a' && cmd <= 'z';
        int eff = rel ? cmd - 32 : cmd;
        if (eff == 'M') eff = 'L';                      /* extra pairs are implicit lines */
        switch (eff) {
        case 'M': case 'L': {
            if (pargs(&P, v, 2)) goto done;
            double nx = rel ? P.cx + DXY(R, v[0]) : RX(R, v[0]);
            double ny = rel ? P.cy + DXY(R, v[1]) : RY(R, v[1]);
            if (cmd == 'M' || cmd == 'm') {             /* genuine moveto */
                if (have) close_sub(&P);                /* fill implies closed subpath */
                P.cx = P.sx = nx; P.cy = P.sy = ny;
                have = 1;
                cmd = rel ? 'l' : 'L';                  /* further pairs are linetos */
            } else {
                edge_add(R, P.cx, P.cy, nx, ny);
                P.cx = nx; P.cy = ny;
            }
            last = 'L';
            break;
        }
        case 'H': {
            if (pargs(&P, v, 1)) goto done;
            double nx = rel ? P.cx + DXY(R, v[0]) : RX(R, v[0]);
            edge_add(R, P.cx, P.cy, nx, P.cy);
            P.cx = nx;
            last = 'L';
            break;
        }
        case 'V': {
            if (pargs(&P, v, 1)) goto done;
            double ny = rel ? P.cy + DXY(R, v[0]) : RY(R, v[0]);
            edge_add(R, P.cx, P.cy, P.cx, ny);
            P.cy = ny;
            last = 'L';
            break;
        }
        case 'C': case 'S': {
            if (pargs(&P, v, eff == 'C' ? 6 : 4)) goto done;
            double x1, y1, x2, y2, x3, y3;
            if (eff == 'C') {
                x1 = rel ? P.cx + DXY(R, v[0]) : RX(R, v[0]);
                y1 = rel ? P.cy + DXY(R, v[1]) : RY(R, v[1]);
                x2 = rel ? P.cx + DXY(R, v[2]) : RX(R, v[2]);
                y2 = rel ? P.cy + DXY(R, v[3]) : RY(R, v[3]);
                x3 = rel ? P.cx + DXY(R, v[4]) : RX(R, v[4]);
                y3 = rel ? P.cy + DXY(R, v[5]) : RY(R, v[5]);
            } else {
                x1 = last == 'C' ? 2 * P.cx - P.pcx : P.cx;
                y1 = last == 'C' ? 2 * P.cy - P.pcy : P.cy;
                x2 = rel ? P.cx + DXY(R, v[0]) : RX(R, v[0]);
                y2 = rel ? P.cy + DXY(R, v[1]) : RY(R, v[1]);
                x3 = rel ? P.cx + DXY(R, v[2]) : RX(R, v[2]);
                y3 = rel ? P.cy + DXY(R, v[3]) : RY(R, v[3]);
            }
            P.pcx = x2; P.pcy = y2;
            cub4(&P, x1, y1, x2, y2, x3, y3, 0);
            P.cx = x3; P.cy = y3;                       /* cub4 leaves cx at entry point */
            last = 'C';
            break;
        }
        case 'Q': case 'T': {
            if (pargs(&P, v, eff == 'Q' ? 4 : 2)) goto done;
            double qx, qy, x3, y3;
            if (eff == 'Q') {
                qx = rel ? P.cx + DXY(R, v[0]) : RX(R, v[0]);
                qy = rel ? P.cy + DXY(R, v[1]) : RY(R, v[1]);
                x3 = rel ? P.cx + DXY(R, v[2]) : RX(R, v[2]);
                y3 = rel ? P.cy + DXY(R, v[3]) : RY(R, v[3]);
            } else {
                qx = last == 'Q' ? 2 * P.cx - P.pqx : P.cx;
                qy = last == 'Q' ? 2 * P.cy - P.pqy : P.cy;
                x3 = rel ? P.cx + DXY(R, v[0]) : RX(R, v[0]);
                y3 = rel ? P.cy + DXY(R, v[1]) : RY(R, v[1]);
            }
            P.pqx = qx; P.pqy = qy;
            quad(&P, qx, qy, x3, y3);
            P.cx = x3; P.cy = y3;
            last = 'Q';
            break;
        }
        case 'A': {
            if (pargs(&P, v, 7)) goto done;
            double x3 = rel ? P.cx + DXY(R, v[5]) : RX(R, v[5]);
            double y3 = rel ? P.cy + DXY(R, v[6]) : RY(R, v[6]);
            arc_to(&P, DXY(R, v[0]), DXY(R, v[1]), v[2],
                   v[3] != 0, v[4] != 0, x3, y3);
            P.cx = x3; P.cy = y3;
            last = 'A';
            break;
        }
        default:
            goto done;                                  /* unsupported command: keep what we have */
        }
    }
done:
    if (have) close_sub(&P);
    fill(R, pc);
}

/* ---- simple shapes ---- */
static void draw_rect(struct rst *R, const struct tag *t, const struct paint *pc)
{
    const uint8_t *v; int vl;
    double x = 0, y = 0, w = 0, h = 0;
    const uint8_t *q;
    if (!attr_get(t, "x", &v, &vl)) { q = v; pnum(&q, v + vl, &x); }
    if (!attr_get(t, "y", &v, &vl)) { q = v; pnum(&q, v + vl, &y); }
    if (!attr_get(t, "width", &v, &vl)) { q = v; pnum(&q, v + vl, &w); }
    if (!attr_get(t, "height", &v, &vl)) { q = v; pnum(&q, v + vl, &h); }
    if (w <= 0 || h <= 0) return;
    double x0 = RX(R, x), y0 = RY(R, y), x1 = RX(R, x + w), y1 = RY(R, y + h);
    edge_add(R, x0, y0, x0, y1);
    edge_add(R, x0, y1, x1, y1);
    edge_add(R, x1, y1, x1, y0);
    edge_add(R, x1, y0, x0, y0);
    fill(R, pc);
}

static void draw_ellipse(struct rst *R, double cx, double cy, double rx, double ry,
                         const struct paint *pc)
{
    if (rx <= 0 || ry <= 0) return;
    double m = rx > ry ? rx : ry;
    int n = (int)(m * 2) + 12;
    if (n > 96) n = 96;
    double px = 0, py = 0;
    for (int i = 0; i <= n; i++) {
        double a = (double)i / n * 2 * DPI;
        double x = cx + rx * dcos(a), y = cy + ry * dsin(a);
        if (i) edge_add(R, px, py, x, y);
        px = x; py = y;
    }
    fill(R, pc);
}

static void draw_circle_el(struct rst *R, const struct tag *t, const struct paint *pc, int ellipse)
{
    const uint8_t *v; int vl;
    double cx = 0, cy = 0, rx = 0, ry = 0;
    const uint8_t *q;
    if (!attr_get(t, "cx", &v, &vl)) { q = v; pnum(&q, v + vl, &cx); }
    if (!attr_get(t, "cy", &v, &vl)) { q = v; pnum(&q, v + vl, &cy); }
    if (ellipse) {
        if (!attr_get(t, "rx", &v, &vl)) { q = v; pnum(&q, v + vl, &rx); }
        if (!attr_get(t, "ry", &v, &vl)) { q = v; pnum(&q, v + vl, &ry); }
    } else {
        if (!attr_get(t, "r", &v, &vl)) { q = v; pnum(&q, v + vl, &rx); }
        ry = rx;
    }
    draw_ellipse(R, RX(R, cx), RY(R, cy), DXY(R, rx), DXY(R, ry), pc);
}

/* Parse a width/height attribute: a bare number or "Npx"; anything else
 * (%, em, ...) counts as absent. Returns 0 on success. */
static int attr_dim(const struct tag *t, const char *name, double *out)
{
    const uint8_t *v; int vl;
    if (attr_get(t, name, &v, &vl)) return -1;
    const uint8_t *p = v, *end = v + vl;
    while (p < end && is_ws(*p)) p++;
    if (pnum(&p, end, out)) return -1;
    while (p < end && is_ws(*p)) p++;
    if (p == end) return 0;
    if (end - p == 2 && p[0] == 'p' && p[1] == 'x') return 0;
    return -1;
}

/* ---- content sniffing ---- */
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

    double dw = 0, dh = 0, vbx = 0, vby = 0, vbw = 0, vbh = 0;
    int has_w = !attr_dim(&root, "width", &dw);
    int has_h = !attr_dim(&root, "height", &dh);
    int has_vb = 0;
    const uint8_t *v; int vl;
    if (!attr_get(&root, "viewBox", &v, &vl)) {
        const uint8_t *q = v, *qe = v + vl;
        if (!pnum(&q, qe, &vbx)) {
            while (q < qe && (is_ws(*q) || *q == ',')) q++;
            if (!pnum(&q, qe, &vby)) {
                while (q < qe && (is_ws(*q) || *q == ',')) q++;
                if (!pnum(&q, qe, &vbw)) {
                    while (q < qe && (is_ws(*q) || *q == ',')) q++;
                    if (!pnum(&q, qe, &vbh) && vbw > 0 && vbh > 0) has_vb = 1;
                }
            }
        }
    }
    if (!has_w && has_vb) { dw = vbw; has_w = 1; }
    if (!has_h && has_vb) { dh = vbh; has_h = 1; }
    if (!has_w && has_h && has_vb) { dw = dh * vbw / vbh; has_w = 1; }
    if (!has_h && has_w && has_vb) { dh = dw * vbh / vbw; has_h = 1; }
    if (!has_w) dw = 300;
    if (!has_h) dh = 150;
    int w = (int)(dw + 0.5), h = (int)(dh + 0.5);
    if (w < 1) w = 1; if (w > 2048) w = 2048;
    if (h < 1) h = 1; if (h > 2048) h = 2048;
    while ((long)w * h > 4L * 1024 * 1024) { if (w >= h) w = (w + 1) / 2; else h = (h + 1) / 2; }

    uint8_t *px = kmalloc((unsigned long)w * h * 4);
    if (!px) return -1;
    for (long i = 0; i < (long)w * h * 4; i++) px[i] = 0;   /* transparent */

    struct rst R;
    R.px = px; R.w = w; R.h = h;
    R.edges = 0; R.ne = 0; R.cape = 0;
    if (has_vb) {
        double sx = w / vbw, sy = h / vbh;
        R.s = sx < sy ? sx : sy;                            /* xMidYMid meet */
        R.ox = (w - vbw * R.s) * 0.5 - vbx * R.s;
        R.oy = (h - vbh * R.s) * 0.5 - vby * R.s;
    } else {
        R.s = 1; R.ox = 0; R.oy = 0;
    }

    /* render walk over the root's content */
    struct paint stk[32];
    int sp = 0;
    stk[0].r = stk[0].g = stk[0].b = 0; stk[0].a = 255;
    stk[0].none = 0; stk[0].evenodd = 0;
    apply_paint(&root, &stk[0]);

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
                apply_paint(&t, &stk[sp]);
            }
            if (t.selfclose && sp > 0) sp--;
            continue;
        }
        if (tag_is(&t, "path")) {
            struct paint pc = stk[sp];
            apply_paint(&t, &pc);
            if (!attr_get(&t, "d", &v, &vl)) parse_path(&R, v, vl, &pc);
            continue;
        }
        if (tag_is(&t, "rect")) {
            struct paint pc = stk[sp];
            apply_paint(&t, &pc);
            draw_rect(&R, &t, &pc);
            continue;
        }
        if (tag_is(&t, "circle") || tag_is(&t, "ellipse")) {
            struct paint pc = stk[sp];
            apply_paint(&t, &pc);
            draw_circle_el(&R, &t, &pc, tag_is(&t, "ellipse"));
            continue;
        }
        if (!t.selfclose) skip_subtree(&cur, end);          /* defs/style/text/... */
    }

    if (R.edges) kfree(R.edges);
    out->w = w; out->h = h; out->rgba = px;
    return 0;
}

void svg_register(void) { img_register(svg_detect, svg_decode); }
