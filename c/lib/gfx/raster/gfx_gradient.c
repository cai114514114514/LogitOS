/* Open Logit -- gradients: the colour-ramp lookup, and the linear and radial span
 * loops that gfx_paint_row dispatches to. The bodies are moved byte for byte from
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
            int span = p->stop[i].t - p->stop[i - 1].t;
            int k = span > 0 ? (t - p->stop[i - 1].t) * 255 / span : 255;
            if (alpha_out)
                *alpha_out = p->stop[i - 1].alpha +
                             (p->stop[i].alpha - p->stop[i - 1].alpha) * k / 255;
            return gfx_mix(p->stop[i - 1].color, p->stop[i].color, k);
        }
    }
    if (alpha_out) *alpha_out = p->stop[p->nstop - 1].alpha;
    return p->stop[p->nstop - 1].color;
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
            int ti = (int)(t < 0 ? 0 : (t > GFX_MONE ? GFX_MONE : t));
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
            int ti = (int)(t > GFX_MONE ? GFX_MONE : t);
            int a;
            unsigned col = gfx_paint_sample(p, ti, &a);
            gfx_over(drow + x * 4, GFX_R(col), GFX_G(col), GFX_B(col), a * ga / 255, c);
        }
        return;
}
