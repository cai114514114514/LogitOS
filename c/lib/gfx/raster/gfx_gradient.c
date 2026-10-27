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

void gfx_gradient_linear_row(const struct gfx_paint *p, unsigned char *drow, int y,
                             int x0, int x1, const unsigned char *cov, int cov_ox)
{
    int ga = p->global_alpha;
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
#define CONIC_TURN (360 * 256)

void gfx_gradient_conic_row(const struct gfx_paint *p, unsigned char *drow, int y,
                            int x0, int x1, const unsigned char *cov, int cov_ox)
{
    int ga = p->global_alpha;
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
