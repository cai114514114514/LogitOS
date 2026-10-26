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

void gfx_image_row(const struct gfx_paint *p, unsigned char *drow, int y,
                   int x0, int x1, const unsigned char *cov, int cov_ox)
{
    int ga = p->global_alpha;
        for (int x = x0; x < x1; x++) {
            int c = cov[x - cov_ox];
            if (!c) continue;
            int r, g, b, a;
            gfx_sample_image(p, (x << 8) + 128, (y << 8) + 128, &r, &g, &b, &a);
            if (a) gfx_over(drow + x * 4, r, g, b, a * ga / 255, c);
        }
}
