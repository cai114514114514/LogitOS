/* ol_display fill/stroke of gfx paths with any gfx paint onto packed targets.
 * See openlogit_display_path.h. Freestanding: no libc, no malloc, no float.
 *
 * Scratch (all from display_scratch(ctx, OL_DS_PATH), so per display):
 *     [ RGBA row, (cx1) * 4 ][ opacity-scaled coverage row, tw ][ A8 band, tw * bh ]
 * The RGBA row is indexed by ABSOLUTE display x because the gfx span functions
 * address `drow + x*4`; no out-of-range pointer is formed. The raster workspace
 * is gfx_raster's own legacy_workspaces[gfx_ctx()]. */
#include "openlogit_display_path.h"
#include "display_pixels.h"
#include "gfx_paint_internal.h"

#define DP_TILE_W 2048          /* coverage tile width, <= GFX_MAX_W */
#define DP_BAND_BYTES 32768     /* A8 band budget */

/* The scissor in display pixels. 0 when the target is unbound or empty. */
static int dp_scissor(const struct ol_display *d, int *x0, int *y0, int *x1, int *y1)
{
    const struct ol_pixel_target *t = &d->target;
    if (!t->px || t->w <= 0 || t->h <= 0) return 0;
    *x0 = 0; *y0 = 0; *x1 = t->w; *y1 = t->h;
    if (t->clip_on) {
        if (t->clx0 > *x0) *x0 = t->clx0;
        if (t->cly0 > *y0) *y0 = t->cly0;
        if (t->clx1 < *x1) *x1 = t->clx1;
        if (t->cly1 < *y1) *y1 = t->cly1;
    }
    return *x0 < *x1 && *y0 < *y1;
}

/* Composite one coverage row (cov[0] is display x `cx`) over [x0,x1) of row y. */
static void dp_row(struct ol_display *d, int y, int x0, int x1,
                   const unsigned char *cov, int cx,
                   const struct gfx_paint *p, unsigned char *rgba)
{
    uint32_t *drow = d->target.px + (long)y * d->target.w;
    int any = 0;
    for (int x = x0; x < x1; x++) {
        if (!cov[x - cx]) continue;
        uint32_t px = drow[x];
        unsigned char *q = rgba + x * 4;
        q[0] = (unsigned char)((px >> d->rpos) & 255);
        q[1] = (unsigned char)((px >> d->gpos) & 255);
        q[2] = (unsigned char)((px >> d->bpos) & 255);
        q[3] = 255;
        any = 1;
    }
    if (!any) return;
#ifdef OL_DISPLAY_PATH_IGNORE_PAINT
    /* negctl: every paint becomes a solid of its first colour */
    struct gfx_paint solid;
    gfx_paint_solid(&solid, p->kind == GFX_SOLID ? p->color : p->stop[0].color, 255);
    p = &solid;
#endif
    if (p->kind == GFX_SOLID) {
        int r = GFX_R(p->color), g = GFX_G(p->color), b = GFX_B(p->color);
        int a = p->alpha * p->global_alpha / 255;
        for (int x = x0; x < x1; x++) {
            int c = cov[x - cx];
            if (c) gfx_over(rgba + x * 4, r, g, b, a, c);
        }
    } else if (p->kind == GFX_LINEAR) {
        gfx_gradient_linear_row(p, rgba, y, x0, x1, cov, cx);
    } else if (p->kind == GFX_RADIAL) {
        gfx_gradient_radial_row(p, rgba, y, x0, x1, cov, cx);
    } else if (p->kind == GFX_IMAGE) {
        gfx_image_row(p, rgba, y, x0, x1, cov, cx);
    }
    for (int x = x0; x < x1; x++) {
        if (!cov[x - cx]) continue;
        const unsigned char *q = rgba + x * 4;
        drow[x] = display_rgb(d, q[0], q[1], q[2]);
    }
}

static int dp_opacity(int o) { return o < 0 ? 0 : o > 255 ? 255 : o; }

/* Rows [y0,y1) x columns [x0,x1) of `mask` (pitch stride, mask[0] at display
 * (mx,my)) with opacity folded in via `tmp`. */
static void dp_mask_rows(struct ol_display *d, const uint8_t *mask, int mx, int my, int stride,
                         int x0, int y0, int x1, int y1, const struct gfx_paint *p,
                         int opacity, unsigned char *rgba, unsigned char *tmp)
{
    for (int y = y0; y < y1; y++) {
        const unsigned char *row = mask + (long)(y - my) * stride;   /* row[0] = display x mx */
        if (opacity < 255) {
            for (int x = x0; x < x1; x++)
                tmp[x - x0] = (unsigned char)((row[x - mx] * opacity + 127) / 255);
            dp_row(d, y, x0, x1, tmp, x0, p, rgba);
        } else {
            dp_row(d, y, x0, x1, row, mx, p, rgba);
        }
    }
}

int ol_display_fill_mask(struct ol_display *d, int x, int y, const uint8_t *mask,
                         int w, int h, int stride,
                         const struct gfx_paint *p, int opacity)
{
    if (!d || !mask || !p || w < 0 || h < 0 || stride < w) return OL_ARGUMENT;
    int sx0, sy0, sx1, sy1;
    if (w == 0 || h == 0 || !dp_scissor(d, &sx0, &sy0, &sx1, &sy1)) return OL_OK;
    opacity = dp_opacity(opacity);
    if (!opacity) return OL_OK;
    int x0 = x > sx0 ? x : sx0, y0 = y > sy0 ? y : sy0;
    int x1 = x + w < sx1 ? x + w : sx1, y1 = y + h < sy1 ? y + h : sy1;
    if (x0 >= x1 || y0 >= y1) return OL_OK;
    unsigned long rowb = (unsigned long)x1 * 4, tmpb = (unsigned long)(x1 - x0);
    unsigned char *rgba = display_scratch(d, OL_DS_PATH, rowb + tmpb);
    if (!rgba) return OL_LIMIT;
    dp_mask_rows(d, mask, x, y, stride, x0, y0, x1, y1, p, opacity, rgba, rgba + rowb);
    return OL_OK;
}

int ol_display_fill_path(struct ol_display *d, const struct gfx_path *path, int rule,
                         const struct gfx_paint *p, int opacity, int subs)
{
    if (!d || !path || !p) return OL_ARGUMENT;
    if (path->overflow) return OL_LIMIT;
    int sx0, sy0, sx1, sy1, bx0, by0, bx1, by1;
    if (!dp_scissor(d, &sx0, &sy0, &sx1, &sy1)) return OL_OK;
    opacity = dp_opacity(opacity);
    if (!opacity) return OL_OK;
    if (!gfx_path_bounds(path, &bx0, &by0, &bx1, &by1)) return OL_OK;
    if (bx0 > sx0) sx0 = bx0;
    if (by0 > sy0) sy0 = by0;
    if (bx1 < sx1) sx1 = bx1;
    if (by1 < sy1) sy1 = by1;
    if (sx0 >= sx1 || sy0 >= sy1) return OL_OK;
    if (subs <= 0) subs = GFX_SUBS;

    int tw = sx1 - sx0 < DP_TILE_W ? sx1 - sx0 : DP_TILE_W;
    int bh = DP_BAND_BYTES / tw;
    if (bh < 1) bh = 1;
    if (bh > sy1 - sy0) bh = sy1 - sy0;
    unsigned long rowb = (unsigned long)sx1 * 4, tmpb = (unsigned long)tw;
    unsigned char *rgba = display_scratch(d, OL_DS_PATH, rowb + tmpb + (unsigned long)tw * bh);
    if (!rgba) return OL_LIMIT;
    unsigned char *tmp = rgba + rowb, *band = tmp + tmpb;

    for (int ty = sy0; ty < sy1; ty += bh) {
        int th = sy1 - ty < bh ? sy1 - ty : bh;
        for (int tx = sx0; tx < sx1; tx += tw) {
            int cw = sx1 - tx < tw ? sx1 - tx : tw;
            if (!gfx_fill_mask_subs(path, rule, band, cw, th, tx, ty, subs)) return OL_LIMIT;
            /* the band buffer has pitch cw */
            dp_mask_rows(d, band, tx, ty, cw, tx, ty, tx + cw, ty + th, p, opacity, rgba, tmp);
        }
    }
    return OL_OK;
}

int ol_display_stroke_path(struct ol_display *d, const struct gfx_path *path,
                           const struct gfx_stroke *st, struct gfx_path *outline,
                           const struct gfx_paint *p, int opacity)
{
    if (!d || !path || !st || !outline || !p) return OL_ARGUMENT;
    gfx_path_reset(outline);
    if (!gfx_stroke_path(outline, path, st)) return st->width <= 0 ? OL_ARGUMENT : OL_LIMIT;
    return ol_display_fill_path(d, outline, GFX_NONZERO, p, opacity, 0);
}
