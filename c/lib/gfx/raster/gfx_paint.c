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
#include "openlogit_sw.h"
#include "gfx_paint_internal.h"

void gfx_surface_init(struct gfx_surface *s, unsigned char *px, int w, int h, int stride)
{
    s->px = px; s->w = w; s->h = h;
    s->stride = stride > 0 ? stride : w * 4;
}

void ol_sw_surface_clear(struct gfx_surface *s)
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
#ifdef GFX_HEADER_NEGCTL_CTOR
    p->flags = 1;   /* control only: the constructor must leave this 0 */
#endif
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

void gfx_paint_conic(struct gfx_paint *p, int cx, int cy, int start_deg24_8)
{
    paint_base(p);
    p->kind = GFX_CONIC;
    p->cx = cx; p->cy = cy; p->r = start_deg24_8;
}

void gfx_paint_radial2(struct gfx_paint *p, int fx, int fy, int fr, int cx, int cy, int r)
{
    paint_base(p);
    p->kind = GFX_RADIAL2;
    p->x0 = fx; p->y0 = fy; p->x1 = fr > 0 ? fr : 0;
    p->cx = cx; p->cy = cy; p->r = r > 0 ? r : 0;
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


/* ------------------------------------------------------------ span paint -- */

/* op != 0: blend modes and Porter-Duff operators (gfx_blend.h). Kept out of the
 * src-over loops below so op 0 pays one branch per row and nothing per pixel.
 * A gradient or image row writes src-over onto a transparent staging chunk, which
 * leaves exactly (paint colour, paint alpha * coverage) per covered pixel; each
 * of those is then composed onto the real row. The staging pointer is biased so
 * the row helpers' absolute `drow + x*4` addressing lands inside the chunk. */
#define GFX_OP_CHUNK 64

static void gfx_paint_row_op(const struct gfx_paint *p, unsigned char *drow, int y,
                             int x0, int x1, const unsigned char *cov, int cov_ox)
{
    enum gfx_op op = (enum gfx_op)p->op;
    if (p->kind == GFX_SOLID) {
        int a = p->alpha * p->global_alpha / 255;
        for (int x = x0; x < x1; x++) {
            int c = cov[x - cov_ox];
            /* CLEAR and friends act with a transparent source too, so no a == 0 skip */
            if (c) gfx_compose(op, drow + x * 4, p->color & 0xFFFFFF, a, c);
        }
        return;
    }
    unsigned char stage[GFX_OP_CHUNK * 4];
    for (int cx = x0; cx < x1; cx += GFX_OP_CHUNK) {
        int ce = cx + GFX_OP_CHUNK < x1 ? cx + GFX_OP_CHUNK : x1;
        gfx_zero(stage, (ce - cx) * 4);
        unsigned char *bias = (unsigned char *)((uintptr_t)stage - (uintptr_t)cx * 4);
        if (p->kind == GFX_LINEAR)      gfx_gradient_linear_row(p, bias, y, cx, ce, cov, cov_ox);
        else if (p->kind == GFX_RADIAL) gfx_gradient_radial_row(p, bias, y, cx, ce, cov, cov_ox);
        else if (p->kind == GFX_CONIC)  gfx_gradient_conic_row(p, bias, y, cx, ce, cov, cov_ox);
        else if (p->kind == GFX_RADIAL2) gfx_gradient_radial2_row(p, bias, y, cx, ce, cov, cov_ox);
        else if (p->kind == GFX_IMAGE)  gfx_image_row(p, bias, y, cx, ce, cov, cov_ox);
        else return;
        for (int x = cx; x < ce; x++) {
            const unsigned char *q = stage + (x - cx) * 4;
            if (!cov[x - cov_ox]) continue;
            gfx_compose(op, drow + x * 4, GFX_RGB(q[0], q[1], q[2]), q[3], 255);
        }
    }
}

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

    if (p->op) { gfx_paint_row_op(p, drow, y, x0, x1, cov, cov_ox); return; }

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
        gfx_gradient_linear_row(p, drow, y, x0, x1, cov, cov_ox);
        return;
    }

    if (p->kind == GFX_RADIAL) {
        gfx_gradient_radial_row(p, drow, y, x0, x1, cov, cov_ox);
        return;
    }

    if (p->kind == GFX_CONIC) {
        gfx_gradient_conic_row(p, drow, y, x0, x1, cov, cov_ox);
        return;
    }

    if (p->kind == GFX_RADIAL2) {
        gfx_gradient_radial2_row(p, drow, y, x0, x1, cov, cov_ox);
        return;
    }

    if (p->kind == GFX_IMAGE) gfx_image_row(p, drow, y, x0, x1, cov, cov_ox);
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
