/* Open Logit -- gradients: the colour-ramp lookup, and the linear, radial and conic
 * span loops that gfx_paint_row dispatches to. The bodies are moved byte for byte from
 * gfx_paint.c; only the function frame around each one is new. */
#include "gfx.h"
#include "gfx_paint_internal.h"

/* -------------------------------------------------------- stop lookup -- */

unsigned gfx_paint_sample(const struct gfx_paint *p, int t, int *alpha_out)
{
    if (t < 0) t = 0;
    if (t > GFX_MONE) t = GFX_MONE;
    if (p->nstop == 0) { if (alpha_out) *alpha_out = p->alpha; return p->color; }
    if (t <= p->stop[0].t) {
        if (alpha_out) *alpha_out = p->stop[0].alpha;
        return p->stop[0].color;
    }
    for (int i = 1; i < p->nstop; i++) {
        if (t <= p->stop[i].t) {
            const struct gfx_stop *s0 = &p->stop[i - 1], *s1 = &p->stop[i];
            int span = s1->t - s0->t;
            int k = span > 0 ? (t - s0->t) * 255 / span : 255;
            int a0 = s0->alpha, a1 = s1->alpha;
            if (alpha_out) *alpha_out = a0 + (a1 - a0) * k / 255;
#ifndef GFX_STOPS_STRAIGHT_LERP
            /* Premultiplied: each stop's colour counts in proportion to its alpha, so
             * a transparent stop contributes no hue. Equal alphas (every opaque
             * ramp) reduce to the straight lerp, so those stay bit-identical. */
            if (a0 != a1) {
                int w0 = a0 * (255 - k), w1 = a1 * k, w = w0 + w1;
                if (w > 0) {
                    int h = w / 2;
                    return GFX_RGB((GFX_R(s0->color) * w0 + GFX_R(s1->color) * w1 + h) / w,
                                   (GFX_G(s0->color) * w0 + GFX_G(s1->color) * w1 + h) / w,
                                   (GFX_B(s0->color) * w0 + GFX_B(s1->color) * w1 + h) / w);
                }
            }
#endif
            return gfx_mix(s0->color, s1->color, k);
        }
    }
    if (alpha_out) *alpha_out = p->stop[p->nstop - 1].alpha;
    return p->stop[p->nstop - 1].color;
}

/* The ramp position for a raw (unclamped, 16.16) t under paint->spread. */
static int spread_t(int spread, long long t)
{
    if (spread == GFX_SPREAD_REPEAT) {
        t %= GFX_MONE;
        return (int)(t < 0 ? t + GFX_MONE : t);
    }
    if (spread == GFX_SPREAD_REFLECT) {
        t %= 2 * GFX_MONE;
        if (t < 0) t += 2 * GFX_MONE;
        return (int)(t > GFX_MONE ? 2 * GFX_MONE - t : t);
    }
    return (int)(t < 0 ? 0 : (t > GFX_MONE ? GFX_MONE : t));
}

#define CONIC_TURN (360 * 256)

/* ------------------------------------------- matrix / dither / two-circle -- */

/* Rows of a paint with GFX_PAINT_HAS_PMAT or GFX_PAINT_DITHER, and every GFX_RADIAL2 row,
 * go through grad_row_ex. The legacy rows below are untouched, so flags 0 stays bit
 * identical. The device pixel centre is sent through the inverse of pmat into paint space
 * (24.8), and the kind-specific parameter t (16.16) is computed there. */

static const unsigned char bayer4[4][4] = {
    { 0,  8,  2, 10}, {12,  4, 14,  6}, { 3, 11,  1,  9}, {15,  7, 13,  5}};

/* The ramp at t with the colour kept in 8.8, interpolated straight from the stops (or
 * premultiplied where their alphas differ, as gfx_paint_sample). Alpha matches
 * gfx_paint_sample exactly. */
static void sample_fx(const struct gfx_paint *p, int t, int c[3], int *alpha)
{
    if (t < 0) t = 0;
    if (t > GFX_MONE) t = GFX_MONE;
    const struct gfx_stop *s0, *s1 = 0;
    if (p->nstop == 0) {
        c[0] = GFX_R(p->color) << 8; c[1] = GFX_G(p->color) << 8; c[2] = GFX_B(p->color) << 8;
        *alpha = p->alpha; return;
    }
    s0 = &p->stop[0];
    if (t > s0->t) {
        s0 = &p->stop[p->nstop - 1];
        for (int i = 1; i < p->nstop; i++)
            if (t <= p->stop[i].t) { s0 = &p->stop[i - 1]; s1 = &p->stop[i]; break; }
    }
    if (!s1) {
        c[0] = GFX_R(s0->color) << 8; c[1] = GFX_G(s0->color) << 8; c[2] = GFX_B(s0->color) << 8;
        *alpha = s0->alpha; return;
    }
    int span = s1->t - s0->t;
    int k = span > 0 ? (t - s0->t) * 255 / span : 255;
    long long kf = span > 0 ? (long long)(t - s0->t) * GFX_MONE / span : GFX_MONE;
    int a0 = s0->alpha, a1 = s1->alpha;
    *alpha = a0 + (a1 - a0) * k / 255;
    long long w0 = (long long)a0 * (GFX_MONE - kf), w1 = (long long)a1 * kf;
    if (a0 == a1 || w0 + w1 <= 0) { w0 = GFX_MONE - kf; w1 = kf; }
    long long w = w0 + w1;
    int c0[3] = {GFX_R(s0->color), GFX_G(s0->color), GFX_B(s0->color)};
    int c1[3] = {GFX_R(s1->color), GFX_G(s1->color), GFX_B(s1->color)};
    for (int i = 0; i < 3; i++)
        c[i] = (int)((((long long)c0[i] * w0 + (long long)c1[i] * w1) * 256 + w / 2) / w);
}

static long long clamp_i(long long v, long long lim) { return v < -lim ? -lim : (v > lim ? lim : v); }

/* Largest t (16.16) of the circle through paint-space point (ux,uy) with radius >= 0.
 * Returns 0 when there is none. All in 24.8 with every input clamped to +-2^21, so the
 * quadratic's terms stay below 2^46 and only the discriminant needs 128 bits. */
static int radial2_t(const struct gfx_paint *p, long long ux, long long uy, long long *t_out)
{
    const long long L = 1LL << 21;
    long long fx = clamp_i(p->x0, L), fy = clamp_i(p->y0, L), fr = clamp_i(p->x1, L);
    long long cx = clamp_i(p->cx, L), cy = clamp_i(p->cy, L), r = clamp_i(p->r, L);
    long long cdx = cx - fx, cdy = cy - fy, dr = r - fr;
    long long pdx = clamp_i(ux - fx, 2 * L), pdy = clamp_i(uy - fy, 2 * L);
    long long a = cdx * cdx + cdy * cdy - dr * dr;
    long long b = pdx * cdx + pdy * cdy + fr * dr;
    long long c0 = pdx * pdx + pdy * pdy - fr * fr;
    long long t1, t2;
    if (a == 0) {
        if (b == 0) return 0;
        t1 = t2 = clamp_i(c0, 1LL << 44) * GFX_MONE / (2 * b);
    } else {
        __int128 disc = (__int128)b * b - (__int128)a * c0;
        if (disc < 0) return 0;
        unsigned __int128 d = (unsigned __int128)disc;
        int sh = 0;
        while (d >> 64) { d >>= 2; sh++; }
        long long s = (long long)gfx_isqrt((unsigned long long)d) << sh;
        long long n[2] = {b + s, b - s}, t[2];
        for (int i = 0; i < 2; i++) {
            long long q = n[i] / a, rem = n[i] % a;   /* t = n/a in 16.16, without n*65536 */
            t[i] = clamp_i(q, 1LL << 30) * GFX_MONE + rem * GFX_MONE / a;
        }
        t1 = t[0] > t[1] ? t[0] : t[1];
        t2 = t[0] > t[1] ? t[1] : t[0];
    }
    t1 = t1 < -(1LL << 40) ? -(1LL << 40) : (t1 > (1LL << 40) ? (1LL << 40) : t1);
    t2 = t2 < -(1LL << 40) ? -(1LL << 40) : (t2 > (1LL << 40) ? (1LL << 40) : t2);
    if (fr * GFX_MONE + t1 * dr >= 0) { *t_out = t1; return 1; }
    if (fr * GFX_MONE + t2 * dr >= 0) { *t_out = t2; return 1; }
    return 0;
}

static void grad_row_ex(const struct gfx_paint *p, unsigned char *drow, int y,
                        int x0, int x1, const unsigned char *cov, int cov_ox)
{
    int ga = p->global_alpha;
    struct gfx_matrix inv;
    int ident = 1;
#ifndef GFX_GRADIENT_IGNORE_PMAT
    if (p->flags & GFX_PAINT_HAS_PMAT) {
        if (!gfx_m_invert(&inv, &p->pmat)) return;       /* singular: nothing to paint */
        ident = 0;
    }
#endif
    int dither = (p->flags & GFX_PAINT_DITHER) != 0;
    long long dx = p->x1 - p->x0, dy = p->y1 - p->y0, len2 = dx * dx + dy * dy;
    if (p->kind == GFX_LINEAR && len2 == 0) return;
    long long start = p->r % CONIC_TURN;
    for (int x = x0; x < x1; x++) {
        int c = cov[x - cov_ox];
        if (!c) continue;
        long long X = ((long long)x << 8) + 128, Y = ((long long)y << 8) + 128, ux = X, uy = Y;
        if (!ident) {
            ux = gfx_m_mulfix((long long)inv.a * X + (long long)inv.c * Y) + inv.e;
            uy = gfx_m_mulfix((long long)inv.b * X + (long long)inv.d * Y) + inv.f;
        }
        long long t;
        if (p->kind == GFX_LINEAR) {
            t = ((ux - p->x0) * dx + (uy - p->y0) * dy) * GFX_MONE / len2;
            t = spread_t(p->spread, t);
        } else if (p->kind == GFX_RADIAL) {
            long long qx = ux - p->cx, qy = uy - p->cy;
            long long d = (long long)gfx_isqrt((unsigned long long)(qx * qx + qy * qy));
            t = spread_t(p->spread, d * GFX_MONE / p->r);
        } else if (p->kind == GFX_CONIC) {
            long long qx = ux - p->cx, qy = uy - p->cy;
            while (qx > 0x3FFFFFFF || qx < -0x3FFFFFFF || qy > 0x3FFFFFFF || qy < -0x3FFFFFFF) {
                qx /= 2; qy /= 2;
            }
            long long a = ((long long)gfx_atan2((int)qy, (int)qx) - start) % CONIC_TURN;
            if (a < 0) a += CONIC_TURN;
            t = a * GFX_MONE / CONIC_TURN;
            if (p->spread == GFX_SPREAD_REFLECT)
                t = t <= GFX_MONE / 2 ? 2 * t : 2 * (GFX_MONE - t);
        } else {                                          /* GFX_RADIAL2 */
            long long raw;
            if (!radial2_t(p, ux, uy, &raw)) continue;
            t = spread_t(p->spread, raw);
        }
        int al, ch[3];
        if (dither) {
            sample_fx(p, (int)t, ch, &al);
            int off = (2 * bayer4[y & 3][x & 3] - 15) * 8;       /* +-0.47 LSB in 1/256 */
            for (int i = 0; i < 3; i++) {
                int v = ch[i] + off + 128;
                ch[i] = v < 0 ? 0 : (v > 255 * 256 + 255 ? 255 : v >> 8);
            }
        } else {
            unsigned col = gfx_paint_sample(p, (int)t, &al);
            ch[0] = GFX_R(col); ch[1] = GFX_G(col); ch[2] = GFX_B(col);
        }
        gfx_over(drow + x * 4, ch[0], ch[1], ch[2], al * ga / 255, c);
    }
}

void gfx_gradient_radial2_row(const struct gfx_paint *p, unsigned char *drow, int y,
                              int x0, int x1, const unsigned char *cov, int cov_ox)
{
    grad_row_ex(p, drow, y, x0, x1, cov, cov_ox);
}

void gfx_gradient_linear_row(const struct gfx_paint *p, unsigned char *drow, int y,
                             int x0, int x1, const unsigned char *cov, int cov_ox)
{
    int ga = p->global_alpha;
    if (p->flags & (GFX_PAINT_HAS_PMAT | GFX_PAINT_DITHER)) {
        grad_row_ex(p, drow, y, x0, x1, cov, cov_ox);
        return;
    }
        long long dx = p->x1 - p->x0, dy = p->y1 - p->y0;
        long long len2 = dx * dx + dy * dy;
        if (len2 == 0) return;
        long long px = ((long long)x0 << 8) + 128 - p->x0;
        long long py = ((long long)y << 8) + 128 - p->y0;
        long long t = (px * dx + py * dy) * GFX_MONE / len2;
        long long dt = (256LL * dx) * GFX_MONE / len2;      /* per device pixel */
        for (int x = x0; x < x1; x++, t += dt) {
            int c = cov[x - cov_ox];
            if (!c) continue;
            int ti = spread_t(p->spread, t);
            int a;
            unsigned col = gfx_paint_sample(p, ti, &a);
            gfx_over(drow + x * 4, GFX_R(col), GFX_G(col), GFX_B(col), a * ga / 255, c);
        }
        return;
}

void gfx_gradient_radial_row(const struct gfx_paint *p, unsigned char *drow, int y,
                             int x0, int x1, const unsigned char *cov, int cov_ox)
{
    int ga = p->global_alpha;
    if (p->flags & (GFX_PAINT_HAS_PMAT | GFX_PAINT_DITHER)) {
        grad_row_ex(p, drow, y, x0, x1, cov, cov_ox);
        return;
    }
        long long py = ((long long)y << 8) + 128 - p->cy;
        for (int x = x0; x < x1; x++) {
            int c = cov[x - cov_ox];
            if (!c) continue;
            long long px = ((long long)x << 8) + 128 - p->cx;
            long long d = (long long)gfx_isqrt((unsigned long long)(px * px + py * py));
            long long t = d * GFX_MONE / p->r;
            int ti = spread_t(p->spread, t);
            int a;
            unsigned col = gfx_paint_sample(p, ti, &a);
            gfx_over(drow + x * 4, GFX_R(col), GFX_G(col), GFX_B(col), a * ga / 255, c);
        }
        return;
}

/* Conic: t is the clockwise angle (y down, as gfx_atan2) from the start angle, a
 * full turn being 0..1. gfx_atan2 takes ints, so a pixel more than ~2^30 24.8
 * units from the centre is scaled down first; only the direction matters. */
void gfx_gradient_conic_row(const struct gfx_paint *p, unsigned char *drow, int y,
                            int x0, int x1, const unsigned char *cov, int cov_ox)
{
    int ga = p->global_alpha;
    if (p->flags & (GFX_PAINT_HAS_PMAT | GFX_PAINT_DITHER)) {
        grad_row_ex(p, drow, y, x0, x1, cov, cov_ox);
        return;
    }
    long long start = p->r % CONIC_TURN;
    long long py = ((long long)y << 8) + 128 - p->cy;
    for (int x = x0; x < x1; x++) {
        int c = cov[x - cov_ox];
        if (!c) continue;
        long long px = ((long long)x << 8) + 128 - p->cx, qy = py;
        while (px > 0x3FFFFFFF || px < -0x3FFFFFFF || qy > 0x3FFFFFFF || qy < -0x3FFFFFFF) {
            px /= 2; qy /= 2;
        }
        long long a = ((long long)gfx_atan2((int)qy, (int)px) - start) % CONIC_TURN;
        if (a < 0) a += CONIC_TURN;
        long long t = a * GFX_MONE / CONIC_TURN;
        if (p->spread == GFX_SPREAD_REFLECT)
            t = t <= GFX_MONE / 2 ? 2 * t : 2 * (GFX_MONE - t);
        int al;
        unsigned col = gfx_paint_sample(p, (int)t, &al);
        gfx_over(drow + x * 4, GFX_R(col), GFX_G(col), GFX_B(col), al * ga / 255, c);
    }
}
