/* Open Logit -- image paint sampling: the nearest, bilinear and clamped-bilinear
 * filters (gfx_sample_image), and the image span loop (gfx_image_row). The bodies are
 * moved byte for byte from gfx_paint.c. The one rename, sample_image to
 * gfx_sample_image, keeps the helper off the generic global namespace. */
#include "gfx.h"
#include "gfx_paint_internal.h"

/* Sample the image at device-space (x,y) given in 24.8. Outside the image is
 * TRANSPARENT, not clamped: clamping smears the edge row across the rest of the
 * fill, which reads as a bug rather than as an image that ended. */
void gfx_sample_image(const struct gfx_paint *p, int x, int y,
                         int *r, int *g, int *b, int *a)
{
    int u, v;
    gfx_m_apply(&p->inv, x, y, &u, &v);
    *r = *g = *b = *a = 0;
    if (p->bilinear == GFX_FILTER_LINEAR_CLAMP) {
        /* Atlas quads provide their own coverage. Clamp texel centers to the
         * crop and interpolate premultiplied color: transparent RGB must not
         * produce a colored fringe, and border samples must not fade to zero.
         * Legacy transparent-border sampling below keeps its existing result. */
        if (u < 128) u = 128;
        if (v < 128) v = 128;
        if (u > p->iw*256-128) u = p->iw*256-128;
        if (v > p->ih*256-128) v = p->ih*256-128;
        int ix = (u-128)>>8, iy = (v-128)>>8;
        int tx = (u-128)&255, ty = (v-128)&255;
        unsigned long long colors[3] = {0,0,0};
        unsigned alpha = 0;
        for (int j=0; j<2; j++) {
            int sy = iy+j < p->ih ? iy+j : p->ih-1;
            for (int i=0; i<2; i++) {
                int sx = ix+i < p->iw ? ix+i : p->iw-1;
                unsigned weight = (i?tx:256-tx)*(j?ty:256-ty);
                const unsigned char *pixel = p->img+(long)sy*p->istride+sx*4;
                unsigned weighted_alpha = pixel[3]*weight;
                alpha += weighted_alpha;
                for (unsigned channel=0; channel<3; channel++)
                    colors[channel] += (unsigned long long)pixel[channel]*weighted_alpha;
            }
        }
        if (alpha) {
            *r = (int)((colors[0]+alpha/2)/alpha);
            *g = (int)((colors[1]+alpha/2)/alpha);
            *b = (int)((colors[2]+alpha/2)/alpha);
            *a = (alpha+32768)/65536;
        }
        return;
    }
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

/* ------------------------------------------------- spread and BOX samplers -- */
/* Outside-image policy for a GFX_IMAGE paint, from spread and flags:
 *   0 transparent (legacy: spread PAD, flags 0)   1 repeat   2 reflect   3 clamp edge. */
enum { IM_TRANSPARENT = 0, IM_REPEAT = 1, IM_REFLECT = 2, IM_CLAMP = 3 };

static int image_mode(const struct gfx_paint *p)
{
    if (p->spread == GFX_SPREAD_REPEAT)  return IM_REPEAT;
    if (p->spread == GFX_SPREAD_REFLECT) return IM_REFLECT;
    return (p->flags & GFX_PAINT_IMG_CLAMP) ? IM_CLAMP : IM_TRANSPARENT;
}

/* Texel index i (any int) mapped into 0..n-1 by the mode, or -1 for "transparent". */
static int wrap_index(int i, int n, int mode)
{
    if (mode == IM_CLAMP) return i < 0 ? 0 : (i >= n ? n - 1 : i);
    if (mode == IM_TRANSPARENT) return (i < 0 || i >= n) ? -1 : i;
    int m = i % (mode == IM_REFLECT ? 2 * n : n);
    if (m < 0) m += (mode == IM_REFLECT ? 2 * n : n);
    if (mode == IM_REFLECT && m >= n) m = 2 * n - 1 - m;
    return m;
}

static void sample_spread(const struct gfx_paint *p, int mode, int u, int v,
                          int *r, int *g, int *b, int *a)
{
    *r = *g = *b = *a = 0;
    if (!p->bilinear) {
        int ix = wrap_index(u >> 8, p->iw, mode), iy = wrap_index(v >> 8, p->ih, mode);
        if (ix < 0 || iy < 0) return;
        const unsigned char *s = p->img + (long)iy * p->istride + ix * 4;
        *r = s[0]; *g = s[1]; *b = s[2]; *a = s[3];
        return;
    }
    /* Bilinear over wrapped taps, premultiplied so transparent texels leave no fringe. */
    int fu = u - 128, fv = v - 128;
    int ix = fu >> 8, iy = fv >> 8, tx = fu & 255, ty = fv & 255;
    unsigned long long col[3] = {0, 0, 0};
    unsigned long long alpha = 0;
    for (int j = 0; j < 2; j++) {
        int sy = wrap_index(iy + j, p->ih, mode);
        for (int i = 0; i < 2; i++) {
            int sx = wrap_index(ix + i, p->iw, mode);
            if (sx < 0 || sy < 0) continue;
            unsigned weight = (i ? tx : 256 - tx) * (j ? ty : 256 - ty);
            const unsigned char *s = p->img + (long)sy * p->istride + sx * 4;
            unsigned long long wa = (unsigned long long)s[3] * weight;
            alpha += wa;
            for (int k = 0; k < 3; k++) col[k] += s[k] * wa;
        }
    }
    if (!alpha) return;
    *r = (int)((col[0] + alpha / 2) / alpha);
    *g = (int)((col[1] + alpha / 2) / alpha);
    *b = (int)((col[2] + alpha / 2) / alpha);
    *a = (int)((alpha + 32768) >> 16);
}

/* Area average of the pixel's footprint. The device pixel (x,y) is mapped through the
 * inverse matrix; its bounding box in image space is averaged with exact fractional edge
 * weights (24.8), premultiplied by alpha. Outside the image follows the mode (a transparent
 * outside still counts in the denominator, so a pixel straddling the edge fades). A footprint
 * wider than twice the image is cut to that, which only matters for absurd shrink factors. */
static void sample_box(const struct gfx_paint *p, int mode, int x, int y,
                       int *r, int *g, int *b, int *a)
{
    int cu[4], cv[4];
    gfx_m_apply(&p->inv, x << 8, y << 8, &cu[0], &cv[0]);
    gfx_m_apply(&p->inv, (x + 1) << 8, y << 8, &cu[1], &cv[1]);
    gfx_m_apply(&p->inv, x << 8, (y + 1) << 8, &cu[2], &cv[2]);
    gfx_m_apply(&p->inv, (x + 1) << 8, (y + 1) << 8, &cu[3], &cv[3]);
    int u0 = cu[0], u1 = cu[0], v0 = cv[0], v1 = cv[0];
    for (int k = 1; k < 4; k++) {
        if (cu[k] < u0) u0 = cu[k];
        if (cu[k] > u1) u1 = cu[k];
        if (cv[k] < v0) v0 = cv[k];
        if (cv[k] > v1) v1 = cv[k];
    }
    if (u1 <= u0) u1 = u0 + 1;
    if (v1 <= v0) v1 = v0 + 1;
    *r = *g = *b = *a = 0;
    int i0 = u0 >> 8, i1 = (u1 + 255) / 256 - 1;
    int j0 = v0 >> 8, j1 = (v1 + 255) / 256 - 1;
    int capx = 2 * p->iw + 2, capy = 2 * p->ih + 2;
    int capped = 0;
    if (i1 - i0 + 1 > capx) { i1 = i0 + capx - 1; capped = 1; }
    if (j1 - j0 + 1 > capy) { j1 = j0 + capy - 1; capped = 1; }
    if (mode == IM_TRANSPARENT && !capped) {
        if (i0 < 0) i0 = 0;
        if (j0 < 0) j0 = 0;
        if (i1 >= p->iw) i1 = p->iw - 1;
        if (j1 >= p->ih) j1 = p->ih - 1;
    }
    unsigned long long sa = 0, sc[3] = {0, 0, 0};
    unsigned long long wxsum = 0, wysum = 0;
    for (int j = j0; j <= j1; j++) {
        int top = j * 256 > v0 ? j * 256 : v0, bot = (j + 1) * 256 < v1 ? (j + 1) * 256 : v1;
        int wy = bot - top;
        if (wy <= 0) continue;
        wysum += wy;
        int sy = wrap_index(j, p->ih, mode);
        for (int i = i0; i <= i1; i++) {
            int left = i * 256 > u0 ? i * 256 : u0, right = (i + 1) * 256 < u1 ? (i + 1) * 256 : u1;
            int wx = right - left;
            if (wx <= 0) continue;
            if (j == j0) wxsum += wx;
            int sx = wrap_index(i, p->iw, mode);
            if (sx < 0 || sy < 0) continue;
            const unsigned char *s = p->img + (long)sy * p->istride + sx * 4;
            unsigned long long wa = (unsigned long long)wx * wy * s[3];
            sa += wa;
            for (int k = 0; k < 3; k++) sc[k] += s[k] * wa;
        }
    }
    /* Transparent outside keeps the full footprint as its denominator. */
    if (mode == IM_TRANSPARENT && !capped) { wxsum = (unsigned)(u1 - u0); wysum = (unsigned)(v1 - v0); }
    unsigned long long area = wxsum * wysum;     /* in 1/65536 texel units */
    if (!sa || !area) return;
    *r = (int)((sc[0] + sa / 2) / sa);
    *g = (int)((sc[1] + sa / 2) / sa);
    *b = (int)((sc[2] + sa / 2) / sa);
    *a = (int)((sa + area / 2) / area);
    if (*a > 255) *a = 255;
}

void gfx_image_row(const struct gfx_paint *p, unsigned char *drow, int y,
                   int x0, int x1, const unsigned char *cov, int cov_ox)
{
    int ga = p->global_alpha;
    int mode = image_mode(p);
    /* Sampler choice, once per span: BOX when asked for and the image shrinks by more
     * than 2x, the spread sampler when the paint tiles or clamps, else the legacy one. */
    int box = (p->flags & GFX_PAINT_FILTER_BOX) && gfx_m_scale_of(&p->inv) > 2 * GFX_MONE;
    for (int x = x0; x < x1; x++) {
        int c = cov[x - cov_ox];
        if (!c) continue;
        int r, g, b, a;
        if (box) sample_box(p, mode, x, y, &r, &g, &b, &a);
        else if (mode != IM_TRANSPARENT) {
            int u, v;
            gfx_m_apply(&p->inv, (x << 8) + 128, (y << 8) + 128, &u, &v);
            sample_spread(p, mode, u, v, &r, &g, &b, &a);
        } else gfx_sample_image(p, (x << 8) + 128, (y << 8) + 128, &r, &g, &b, &a);
        if (a) gfx_over(drow + x * 4, r, g, b, a * ga / 255, c);
    }
}
