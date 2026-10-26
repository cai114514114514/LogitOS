/* Private pixel access shared by display primitives and backdrop materials.
 * Keep channel packing and allocation failure reporting identical in both. */
#ifndef OPENLOGIT_DISPLAY_PIXELS_H
#define OPENLOGIT_DISPLAY_PIXELS_H
#include "openlogit_display.h"
#include "gfx.h"

/* Moving the material to its own file otherwise turns every blur/store pack
 * into an external call (seven call sites in the x86 object). Keep the pixel
 * operation inline; the public entry point delegates to this same mapping. */
static inline uint32_t display_rgb(struct ol_display *ctx,
                                   uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)r << ctx->rpos | (uint32_t)g << ctx->gpos |
           (uint32_t)b << ctx->bpos;
}

static inline void *display_alloc(struct ol_display *ctx, unsigned long bytes)
{
    void *p = ctx->allocate ? ctx->allocate(bytes) : 0;
    if (!p) ctx->error = OL_LIMIT;
    return p;
}

static inline void display_free(struct ol_display *ctx, void *p)
{
    if (ctx->release) ctx->release(p);
}

static inline void unpack(struct ol_display *ctx, uint32_t pixel,
                          int *r, int *g, int *b)
{
    *r = (pixel >> ctx->rpos) & 255;
    *g = (pixel >> ctx->gpos) & 255;
    *b = (pixel >> ctx->bpos) & 255;
}

/* Per-display scratch slot (see the rule above struct ol_display). Grows via
 * ctx->allocate, never shrinks, and keeps the old buffer when growth fails
 * (ctx->error = OL_LIMIT, returns NULL). Contents are NOT preserved across a
 * successful growth; callers treat the buffer as scratch. */
#ifdef OL_DISPLAY_SCRATCH_NEGCTL_GLOBAL
static unsigned char ol_ds_global[1u << 16];
#endif
static inline void *display_scratch(struct ol_display *ctx, int slot, unsigned long bytes)
{
    if (!ctx || slot < 0 || slot >= OL_DS_N) return 0;
#ifdef OL_DISPLAY_SCRATCH_NEGCTL_GLOBAL
    if (bytes > sizeof ol_ds_global) { ctx->error = OL_LIMIT; return 0; }
    return ol_ds_global;
#else
    if (ctx->scratch[slot] && ctx->scratch_n[slot] >= bytes) return ctx->scratch[slot];
    void *p = display_alloc(ctx, bytes);
    if (!p) return 0;
    display_free(ctx, ctx->scratch[slot]);
    ctx->scratch[slot] = p;
    ctx->scratch_n[slot] = bytes;
    return p;
#endif
}

/* ---- helpers shared by display_*.c, moved verbatim from openlogit_display.c ---- */
static inline int clip_ij(struct ol_display *ctx, int x, int y, int w, int h, int *i0, int *j0, int *i1, int *j1)
{
    struct ol_pixel_target *s = &ctx->target;
    if (!s->px || w <= 0 || h <= 0) return 0;
    int cx0 = 0, cy0 = 0, cx1 = s->w, cy1 = s->h;
    if (s->clip_on) {
        if (s->clx0 > cx0) cx0 = s->clx0;
        if (s->cly0 > cy0) cy0 = s->cly0;
        if (s->clx1 < cx1) cx1 = s->clx1;
        if (s->cly1 < cy1) cy1 = s->cly1;
    }
    *i0 = cx0 - x; if (*i0 < 0) *i0 = 0;
    *j0 = cy0 - y; if (*j0 < 0) *j0 = 0;
    *i1 = cx1 - x; if (*i1 > w) *i1 = w;
    *j1 = cy1 - y; if (*j1 > h) *j1 = h;
    return *i0 < *i1 && *j0 < *j1;
}

static inline int clip_px(const struct ol_pixel_target *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    if (s->clip_on && (x < s->clx0 || y < s->cly0 || x >= s->clx1 || y >= s->cly1)) return 0;
    return 1;
}

static inline uint32_t display_get(struct ol_display *ctx, int x, int y)
{
    struct ol_pixel_target *s = &ctx->target;
    if (!s->px || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    if (s->clip_on && (x < s->clx0 || y < s->cly0 || x >= s->clx1 || y >= s->cly1))
        return 0;
    return s->px[y * s->w + x];
}

static inline const unsigned char *corner_mask_for(int w, int h, int *rad)
{
    int r = *rad;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { *rad = 0; return 0; }
    *rad = r;
    return gfx_mask_corner(GFX_MASK_FILL, r, r, 0);
}

static inline int corner_cov(int i, int j, int w, int h, int rad, const unsigned char *m)
{
    if (rad <= 0) return 255;
    int cx = -1, cy = -1;
    if (i < rad)            cx = i;
    else if (i >= w - rad)  cx = w - 1 - i;
    if (j < rad)            cy = j;
    else if (j >= h - rad)  cy = h - 1 - j;
    if (cx < 0 || cy < 0) return 255;       /* in one of the straight bands */

    if (!m) {
        /* Radius past GFX_MASK_MAX: the old boolean point sample, i.e. THE
         * STAIRCASE this whole function exists to remove -- kept as the
         * fallback here rather than dropping the corner altogether, because a
         * missing pixel is worse than a sharp one. THIS is the ACCEPTABLE
         * half of gfx_mask_corner's contract (see its comment in
         * gfx_mask.c): complete, no geometry dropped, and no longer silent --
         * every refusal is counted the instant gfx_mask_corner returns NULL
         * (mrefuse / gfx_mask_refused() there), so a run where the desktop
         * chrome keeps hitting this fallback is a number someone can read,
         * not a guess from a screenshot. What this function does NOT do is
         * grow its own buffer to avoid the fallback the way aui.c's BIG_MASK
         * tile does for the toolkit's shapes: that buffer would be KERNEL
         * .bss (this file is compiled into the kernel -- see gfx.h's top
         * comment), and GFX_MASK_MAX was sized deliberately small for
         * exactly that reason. A bigger kernel buffer is a real, measured
         * cost paid by every boot; a corner that is occasionally a staircase
         * on an oversized window-manager shape is not. */
        int dx = rad - cx, dy = rad - cy;
        return dx * dx + dy * dy <= rad * rad ? 255 : 0;
    }
    return m[(long)cy * rad + cx];
}

static inline void blend_cov(struct ol_display *ctx, int x, int y, uint32_t color, int a)
{
    if (a <= 0) return;
    if (a >= 255) { ol_display_put(ctx, x, y, color); return; }
    int cr, cg, cb, br, bg, bb;
    unpack(ctx, color, &cr, &cg, &cb);
    unpack(ctx, display_get(ctx, x, y), &br, &bg, &bb);
    ol_display_put(ctx, x, y, ol_display_rgb(ctx, (uint8_t)((cr * a + br * (255 - a)) / 255),
                        (uint8_t)((cg * a + bg * (255 - a)) / 255),
                        (uint8_t)((cb * a + bb * (255 - a)) / 255)));
}

static inline uint32_t color_lerp(struct ol_display *ctx, uint32_t a, uint32_t b, int num, int den)
{
    int ar, ag, ab, br, bg, bb;
    unpack(ctx, a, &ar, &ag, &ab);
    unpack(ctx, b, &br, &bg, &bb);
    int r = ar + (br - ar) * num / den;
    int g = ag + (bg - ag) * num / den;
    int bl = ab + (bb - ab) * num / den;
    return ol_display_rgb(ctx, (uint8_t)r, (uint8_t)g, (uint8_t)bl);
}
#endif
