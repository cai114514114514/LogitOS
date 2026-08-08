/* Open Logit -- paints and Porter-Duff src-over compositing.
 *
 * A paint answers one question for one device pixel: what colour and what alpha
 * arrives here. Coverage from the rasterizer then modulates that alpha, and the
 * result is composited src-over into a STRAIGHT-alpha RGBA surface -- straight
 * and not premultiplied because that is what SYS_GUI_BLIT consumes, so a
 * finished surface reaches the screen with one blit and no conversion pass.
 *
 * Gradients step INCREMENTALLY along x. Both the linear projection and the
 * image inverse-transform are affine in device space, so the parameter's
 * derivative per pixel is a constant computed once per span rather than a
 * multiply-and-divide per pixel. The radial case cannot: a distance needs its
 * square root, and that is one gfx_isqrt per pixel, which is the honest cost of
 * a radial gradient and the reason not to reach for one when a linear will do.
 */
#include "gfx.h"

void gfx_surface_init(struct gfx_surface *s, unsigned char *px, int w, int h, int stride)
{
    s->px = px; s->w = w; s->h = h;
    s->stride = stride > 0 ? stride : w * 4;
}

void gfx_surface_clear(struct gfx_surface *s)
{
    if (!s || !s->px) return;
    for (int y = 0; y < s->h; y++) gfx_zero(s->px + (long)y * s->stride, s->w * 4);
}

/* src-over onto straight alpha:
 *     aO = aS + aD*(1-aS)
 *     cO = (cS*aS + cD*aD*(1-aS)) / aO
 * The two shortcuts below are not optimisations, they are the cases where the
 * general formula divides by something that can be zero or loses a bit: a fully
 * opaque source replaces outright, and a fully transparent destination cannot
 * contribute a colour to average with. */
void gfx_over(unsigned char *dst, int sr, int sg, int sb, int sa, int cov)
{
    /* ROUND, do not truncate, and round at BOTH divisions. Truncating
     * `da*(255-a)/255` is not a half-bit of error: with da=1 and a=1 it floors
     * the destination's surviving alpha to ZERO, the destination colour drops
     * out of the average entirely, and the result is the source at full
     * strength -- 17/255 off the definition, measured. Everything faint
     * composited over something faint went wrong that way. */
    int a = (sa * cov + 127) / 255;
    if (a <= 0) return;
    if (a >= 255) {
        dst[0] = (unsigned char)sr; dst[1] = (unsigned char)sg;
        dst[2] = (unsigned char)sb; dst[3] = 255;
        return;
    }
    int da = dst[3];
    if (da == 0) {
        dst[0] = (unsigned char)sr; dst[1] = (unsigned char)sg;
        dst[2] = (unsigned char)sb; dst[3] = (unsigned char)a;
        return;
    }
    int ia = 255 - a;
    int back = (da * ia + 127) / 255;         /* destination's surviving alpha */
    int oa = a + back;
    if (oa <= 0) { dst[3] = 0; return; }
    int h = oa / 2;
    dst[0] = (unsigned char)((sr * a + dst[0] * back + h) / oa);
    dst[1] = (unsigned char)((sg * a + dst[1] * back + h) / oa);
    dst[2] = (unsigned char)((sb * a + dst[2] * back + h) / oa);
    dst[3] = (unsigned char)(oa > 255 ? 255 : oa);
}

/* ----------------------------------------------------------- constructors -- */

static void paint_base(struct gfx_paint *p)
{
    gfx_zero(p, (int)sizeof *p);
    p->alpha = 255;
    p->global_alpha = 255;
    gfx_m_identity(&p->inv);
}

void gfx_paint_solid(struct gfx_paint *p, unsigned color, int alpha)
{
    paint_base(p);
    p->kind = GFX_SOLID; p->color = color;
    p->alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
}

void gfx_paint_linear(struct gfx_paint *p, int x0, int y0, int x1, int y1)
{
    paint_base(p);
    p->kind = GFX_LINEAR;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
}

void gfx_paint_radial(struct gfx_paint *p, int cx, int cy, int r)
{
    paint_base(p);
    p->kind = GFX_RADIAL;
    p->cx = cx; p->cy = cy; p->r = r > 0 ? r : 1;
}

/* Stops are kept sorted by position. A caller adding them out of order is not
 * an error worth refusing -- CSS lets a later stop sit before an earlier one and
 * requires it be clamped up -- so they are inserted in place. */
void gfx_paint_stop(struct gfx_paint *p, int t, unsigned color, int alpha)
{
    if (p->nstop >= GFX_MAX_STOPS) return;
    if (t < 0) t = 0;
    if (t > GFX_MONE) t = GFX_MONE;
    int i = p->nstop;
    while (i > 0 && p->stop[i - 1].t > t) { p->stop[i] = p->stop[i - 1]; i--; }
    p->stop[i].t = t; p->stop[i].color = color;
    p->stop[i].alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    p->nstop++;
}

int gfx_paint_image(struct gfx_paint *p, const unsigned char *rgba,
                    int w, int h, int stride, const struct gfx_matrix *img2dev,
                    int bilinear)
{
    paint_base(p);
    if (!rgba || w <= 0 || h <= 0) return 0;
    struct gfx_matrix inv;
    if (img2dev) { if (!gfx_m_invert(&inv, img2dev)) return 0; }
    else gfx_m_identity(&inv);
    p->kind = GFX_IMAGE; p->img = rgba; p->iw = w; p->ih = h;
    p->istride = stride > 0 ? stride : w * 4;
    p->inv = inv; p->bilinear = bilinear;
    return 1;
}

/* --------------------------------------------------------------- sampling -- */

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

/* Sample the image at device-space (x,y) given in 24.8. Outside the image is
 * TRANSPARENT, not clamped: clamping smears the edge row across the rest of the
 * fill, which reads as a bug rather than as an image that ended. */
static void sample_image(const struct gfx_paint *p, int x, int y,
                         int *r, int *g, int *b, int *a)
{
    int u, v;
    gfx_m_apply(&p->inv, x, y, &u, &v);
    *r = *g = *b = *a = 0;
    if (!p->bilinear) {
        int ix = u >> 8, iy = v >> 8;
        if (u < 0 || v < 0 || ix >= p->iw || iy >= p->ih) return;
        const unsigned char *s = p->img + (long)iy * p->istride + ix * 4;
        *r = s[0]; *g = s[1]; *b = s[2]; *a = s[3];
        return;
    }
    /* Bilinear: sample centres sit at pixel centres, so shift by half a pixel
     * before splitting into integer and fraction. Skipping that offset is the
     * classic half-pixel drift that shows up as an image creeping up-left as it
     * is scaled. */
    int fu = u - 128, fv = v - 128;
    int ix = fu >> 8, iy = fv >> 8;
    int tx = fu & 255, ty = fv & 255;
    int acc[4] = { 0, 0, 0, 0 };
    for (int j = 0; j < 2; j++) {
        int sy = iy + j;
        if (sy < 0) sy = 0;
        if (sy >= p->ih) sy = p->ih - 1;
        for (int i = 0; i < 2; i++) {
            int sx = ix + i;
            int inside = (ix + i) >= 0 && (ix + i) < p->iw && (iy + j) >= 0 && (iy + j) < p->ih;
            if (sx < 0) sx = 0;
            if (sx >= p->iw) sx = p->iw - 1;
            int wx = i ? tx : 255 - tx, wy = j ? ty : 255 - ty;
            int wgt = wx * wy;                        /* 0..65025 */
            const unsigned char *s = p->img + (long)sy * p->istride + sx * 4;
            acc[0] += s[0] * wgt; acc[1] += s[1] * wgt; acc[2] += s[2] * wgt;
            acc[3] += (inside ? s[3] : 0) * wgt;
        }
    }
    *r = acc[0] / 65025; *g = acc[1] / 65025; *b = acc[2] / 65025; *a = acc[3] / 65025;
}

/* ------------------------------------------------------------ span paint -- */

void gfx_paint_row(const struct gfx_paint *p, struct gfx_surface *dst, int y,
                   int x0, int x1, const unsigned char *cov, int cov_ox)
{
    if (!p || !dst || !dst->px) return;
    if (y < 0 || y >= dst->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (x1 <= x0) return;
    unsigned char *drow = dst->px + (long)y * dst->stride;
    int ga = p->global_alpha;

    if (p->kind == GFX_SOLID) {
        int r = GFX_R(p->color), g = GFX_G(p->color), b = GFX_B(p->color);
        int a = p->alpha * ga / 255;
        for (int x = x0; x < x1; x++) {
            int c = cov[x - cov_ox];
            if (c) gfx_over(drow + x * 4, r, g, b, a, c);
        }
        return;
    }

    if (p->kind == GFX_LINEAR) {
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

    if (p->kind == GFX_RADIAL) {
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

    if (p->kind == GFX_IMAGE) {
        for (int x = x0; x < x1; x++) {
            int c = cov[x - cov_ox];
            if (!c) continue;
            int r, g, b, a;
            sample_image(p, (x << 8) + 128, (y << 8) + 128, &r, &g, &b, &a);
            if (a) gfx_over(drow + x * 4, r, g, b, a * ga / 255, c);
        }
    }
}

/* -------------------------------------------------------- gradient strips -- */

void gfx_gradient_strip(unsigned char *dst, int n, unsigned c0, unsigned c1, int alpha)
{
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        unsigned c = gfx_mix(c0, c1, n > 1 ? i * 255 / (n - 1) : 0);
        dst[i * 4 + 0] = (unsigned char)GFX_R(c);
        dst[i * 4 + 1] = (unsigned char)GFX_G(c);
        dst[i * 4 + 2] = (unsigned char)GFX_B(c);
        dst[i * 4 + 3] = (unsigned char)alpha;
    }
}

void gfx_gradient_strip_paint(unsigned char *dst, int n, const struct gfx_paint *p)
{
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        int t = n > 1 ? (int)((long long)i * GFX_MONE / (n - 1)) : 0;
        int a;
        unsigned c = gfx_paint_sample(p, t, &a);
        dst[i * 4 + 0] = (unsigned char)GFX_R(c);
        dst[i * 4 + 1] = (unsigned char)GFX_G(c);
        dst[i * 4 + 2] = (unsigned char)GFX_B(c);
        dst[i * 4 + 3] = (unsigned char)(a * p->global_alpha / 255);
    }
}
