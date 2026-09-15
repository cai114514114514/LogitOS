/* OpenLogit opaque display/effect backend. Moved intact from fb.c on
 * 2026-09-13: retain its damage clipping, nine-slice masks, moving-sum blur
 * and glass optimizations. Driver transport is deliberately absent here.
 * Each context owns target/clip and allocator-backed effect scratch. Calls
 * on borrowed targets are serialized by their owner, as are shared coverage
 * and material caches; native ol_device submissions remain independent. */
#include "openlogit_display.h"
#include "openlogit_glass.h"
#include "openlogit_draw.h"
#define blur_scratch ctx->blur_scratch
#define blur_scratch_n ctx->blur_scratch_n
#define glass_buf ctx->glass_buf
#define glass_buf_n ctx->glass_buf_n
#define glass_line ctx->glass_line
#define glass_line_n ctx->glass_line_n
#define fb_shadow_clamped ctx->shadow_clamped
static void *display_alloc(struct ol_display *ctx,unsigned long n)
{void *p=ctx->allocate?ctx->allocate(n):0;if(!p)ctx->error=OL_LIMIT;return p;}
static void display_free(struct ol_display *ctx,void *p){if(ctx->release)ctx->release(p);}
uint32_t ol_display_rgb(struct ol_display *ctx,uint8_t r,uint8_t g,uint8_t b)
{return (uint32_t)r<<ctx->rpos | (uint32_t)g<<ctx->gpos | (uint32_t)b<<ctx->bpos;}
static void unpack(struct ol_display *ctx,uint32_t c,int *r,int *g,int *b)
{*r=(c>>ctx->rpos)&255;*g=(c>>ctx->gpos)&255;*b=(c>>ctx->bpos)&255;}
void ol_display_put(struct ol_display *ctx, int x, int y, uint32_t color)
{
    struct ol_pixel_target *s = &ctx->target;
    if (!s->px || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return;
    if (s->clip_on && (x < s->clx0 || y < s->cly0 || x >= s->clx1 || y >= s->cly1))
        return;
    s->px[y * s->w + x] = color;
}

/* ---- the clip as a LOOP BOUND, not a per-pixel test ------------------------
 *
 * fb_put has always tested the clip and dropped the pixel, which is correct and
 * was cheap enough while the clip was only ever an app's own scissor. It stops
 * being cheap the moment the COMPOSITOR draws the whole scene clipped to a
 * damage rectangle: the work a damage rectangle exists to remove is precisely
 * "iterate every pixel of a 1180x620 window and throw them away".
 *
 * So every rect-shaped primitive below asks for its surviving index range up
 * front. i and j keep their original meaning -- they index the SHAPE, not the
 * screen -- so a gradient row, a rounded corner or a glyph's coverage byte is
 * computed from exactly the same j it always was; only the rows and columns
 * that would have been discarded are never visited. Output is unchanged, which
 * is the property tests/unit/fb_clip_test.c pins down.
 *
 * Returns 0 when nothing survives. */
static int clip_ij(struct ol_display *ctx, int x, int y, int w, int h, int *i0, int *j0, int *i1, int *j1)
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

/* Is this pixel of the CURRENT TARGET writable? For the two primitives that
 * write s->px straight (the blur and the glass, which read a neighbourhood and
 * so cannot be expressed as a clamped loop over their own output). */
static int clip_px(const struct ol_pixel_target *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    if (s->clip_on && (x < s->clx0 || y < s->cly0 || x >= s->clx1 || y >= s->cly1)) return 0;
    return 1;
}

static uint32_t display_get(struct ol_display *ctx, int x, int y)
{
    struct ol_pixel_target *s = &ctx->target;
    if (!s->px || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    if (s->clip_on && (x < s->clx0 || y < s->cly0 || x >= s->clx1 || y >= s->cly1))
        return 0;
    return s->px[y * s->w + x];
}


void ol_display_blit_surface(struct ol_display *ctx, int dx, int dy, const struct ol_pixel_target *src)
{
    struct ol_pixel_target *t = &ctx->target;
    if (!t || !t->px || !src->px) return;
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, dx, dy, src->w, src->h, &i0, &j0, &i1, &j1)) return;
    for (int y = j0; y < j1; y++) {
        const uint32_t *srow = src->px + (uint32_t)y * src->w;
        uint32_t *drow = t->px + (uint32_t)(dy + y) * t->w + dx;
        for (int x = i0; x < i1; x++) drow[x] = srow[x];
    }
}

/* Nearest-neighbour scaled, opaque blit of a surface into dest rect (dx,dy,dw,dh)
 * of the current target. Used for the window open "pop" (scale 0.85->1.0). */
void ol_display_blit_surface_scaled(struct ol_display *ctx, int dx, int dy, int dw, int dh, const struct ol_pixel_target *src)
{
    struct ol_pixel_target *t = &ctx->target;
    if (!t->px || !src->px || dw <= 0 || dh <= 0) return;
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, dx, dy, dw, dh, &i0, &j0, &i1, &j1)) return;
    for (int j = j0; j < j1; j++) {
        int sy = j * src->h / dh;               /* still against the FULL dest rect */
        const uint32_t *srow = src->px + (uint32_t)sy * src->w;
        uint32_t *drow = t->px + (uint32_t)(dy + j) * t->w;
        for (int i = i0; i < i1; i++)
            drow[dx + i] = srow[i * src->w / dw];
    }
}

/* Same contract as fb_blit_surface_scaled, BILINEAR: every dest pixel blends
 * the four source texels around its sample point instead of picking one.
 *
 * WHY NEAREST SHIMMERS AND THIS DOESN'T. Nearest maps dest pixel i to source
 * column i*src->w/dw -- an integer division, so as dw changes frame to frame
 * (a resize drag, the open/close pop's scale 0.85->1.0) the column that
 * division rounds to for a given i jumps discretely: one frame a source row
 * survives into the dest whole, the next it is skipped outright, because the
 * ratio crossed an integer boundary between them. That flip is what "rows pop
 * in and out" IS. A bilinear sample's four weights change continuously with
 * the ratio -- there is no boundary to cross -- so consecutive frames differ
 * by a small blend shift instead of by a row appearing or vanishing.
 *
 * FIXED POINT. `stepx`/`stepy` are 16.16 -- the same texture-step family as
 * Open Logit's transform matrices (gfx.h) -- and walked by simple addition
 * rather than re-deriving i*src->w/dw per pixel, matching how every other
 * per-pixel loop in this file (fb_blur_rect's moving sums, fb_liquid_glass's
 * displacement) turns an O(pixels) divide into an O(pixels) add. The -0.5
 * texel bias centres the sample on the dest pixel's centre rather than its
 * top-left corner, which is what makes a 1:1 scale reproduce the source
 * exactly instead of shifting it half a texel toward the bottom-right.
 *
 * The blend weights are then read off the TOP 8 BITS of each 16-bit fraction
 * (0..255), not the full 16 -- so a weight product (wx*wy) tops out at
 * 255*255=65025, comfortably inside a 32-bit int alongside a 0..255 colour
 * channel, with no 64-bit multiply needed in the innermost loop. Precision
 * lost below bit 8 of the fraction is under 1/256 of a texel step, far finer
 * than a screen pixel can show.
 *
 * ROUNDED, NOT TRUNCATED, on the final divide -- this tree has already found
 * the alternative's failure mode once (fb_liquid_glass's forerunner and
 * gfx_over both floored a /255 and quietly darkened every faint-over-faint
 * blend by up to 1/255 at the low end; see the note above gfx_over). The
 * same shape of bug here would show as a fully-mixed edge fading slightly
 * dark relative to its four source texels, frame after frame, in a way no
 * single screenshot flags but a repeated blend accumulates.
 *
 * NOT a box/area filter: this samples exactly four texels per dest pixel
 * regardless of how far dw has shrunk src->w, so a large downscale still
 * aliases somewhat (a real box filter would average every source texel a
 * dest pixel covers, at a real per-pixel cost that grows with the scale
 * ratio instead of staying flat at four taps). That trade is deliberate --
 * see tests/unit/fb_scale_bl_bench.c, which builds this file host-side (the
 * same stub pattern as fb_clip_test.c) and times both paths on a 1180x620
 * window surface (a real browser/Finder canvas size): bilinear cost 4.3x
 * nearest at a ~0.3x shrink and 4.4x at a ~1.5x grow -- flat regardless of
 * direction, matching "four fetches + three lerps vs. one fetch" rather than
 * scaling with how much the image moved. (Host cycles, not guest TCG --
 * fb.c cannot run under QEMU on the host that builds it -- but the RATIO
 * between two functions measured the same way on the same host is the
 * number that matters here, and it is what decides the trade below.) Cheap
 * enough for the ONE window currently under an open/close pop or a live
 * resize drag; not proposed here for every window a compositor redraws
 * every frame regardless of motion -- that math is 4x the cost for windows
 * that were not asked to look smoother, paid on every frame instead of only
 * the animating one. The same 4x-per-tap-count shape is why fb_liquid_glass
 * above samples R, G and B as three separate single-tap fetches rather than
 * three bilinear ones: a cost that is well spent on the rim band of one
 * glass panel would not be spent the same way if paid by every pixel of it. */
void ol_display_blit_surface_scaled_bl(struct ol_display *ctx, int dx, int dy, int dw, int dh, const struct ol_pixel_target *src)
{
    struct ol_pixel_target *t = &ctx->target;
    if (!t->px || !src->px || dw <= 0 || dh <= 0 || src->w <= 0 || src->h <= 0) return;
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, dx, dy, dw, dh, &i0, &j0, &i1, &j1)) return;

    uint32_t stepx = ((uint32_t)src->w << 16) / (uint32_t)dw;
    uint32_t stepy = ((uint32_t)src->h << 16) / (uint32_t)dh;
    int maxsx = src->w - 1, maxsy = src->h - 1;

    for (int j = j0; j < j1; j++) {
        int64_t sy16 = (int64_t)j * stepy + (int64_t)(stepy >> 1) - 0x8000;
        if (sy16 < 0) sy16 = 0;
        int sy0 = (int)(sy16 >> 16);
        if (sy0 > maxsy) sy0 = maxsy;
        int wy1 = (int)((sy16 >> 8) & 0xFF);      /* top 8 bits of the fraction */
        int sy1 = sy0 < maxsy ? sy0 + 1 : sy0;

        const uint32_t *row0 = src->px + (uint32_t)sy0 * src->w;
        const uint32_t *row1 = src->px + (uint32_t)sy1 * src->w;
        uint32_t *drow = t->px + (uint32_t)(dy + j) * t->w + dx;

        int64_t sxwalk = (int64_t)i0 * stepx + (int64_t)(stepx >> 1) - 0x8000;
        for (int i = i0; i < i1; i++, sxwalk += stepx) {
            int64_t sx16 = sxwalk;
            if (sx16 < 0) sx16 = 0;
            int sx0 = (int)(sx16 >> 16);
            if (sx0 > maxsx) sx0 = maxsx;
            int wx1 = (int)((sx16 >> 8) & 0xFF);
            int sx1 = sx0 < maxsx ? sx0 + 1 : sx0;

            /* Weights past this point are exact for the pixel sampled: when an
             * axis clamped (sx0==maxsx or sy0==maxsy) its "1" tap duplicates
             * the "0" tap (sx1==sx0 / sy1==sy0), so the two texels being
             * blended are identical and any wx1/wy1 split of that axis's 256
             * yields the same sum -- no separate zero-the-weight case needed. */
            uint32_t p00 = row0[sx0], p10 = row0[sx1];
            uint32_t p01 = row1[sx0], p11 = row1[sx1];
            int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
            unpack(ctx, p00, &r00, &g00, &b00);
            unpack(ctx, p10, &r10, &g10, &b10);
            unpack(ctx, p01, &r01, &g01, &b01);
            unpack(ctx, p11, &r11, &g11, &b11);

            /* The four-weight spelling did 16 multiplies per destination
             * pixel: four to build wXY, then four per colour channel. Bilinear
             * interpolation is separable, and a weighted pair can be written
             * exactly as a*256 + (b-a)*weight. Do that twice horizontally and
             * once vertically for each channel: nine multiplies total, with
             * shifts for the three factors of 256. There is deliberately no
             * intermediate divide: rounding between the axes changes pixels.
             * The one final +32768 and >>16 remains algebraically and
             * bit-for-bit the old expression. The largest intermediate is
             * 255*256*256, comfortably inside signed int. */
            int rt = (r00 << 8) + (r10 - r00) * wx1;
            int rb = (r01 << 8) + (r11 - r01) * wx1;
            int gt = (g00 << 8) + (g10 - g00) * wx1;
            int gb = (g01 << 8) + (g11 - g01) * wx1;
            int bt = (b00 << 8) + (b10 - b00) * wx1;
            int bb = (b01 << 8) + (b11 - b01) * wx1;
#ifdef FB_SCALE_BL_NEGCTL_AXIS_ROUND
            /* Test-only mutation: the former tempting two-pass spelling loses
             * the low eight horizontal bits before the vertical blend. */
            rt = ((rt + 128) >> 8) << 8;
#endif
            int r = ((rt << 8) + (rb - rt) * wy1 + 32768) >> 16;
            int g = ((gt << 8) + (gb - gt) * wy1 + 32768) >> 16;
            int b = ((bt << 8) + (bb - bt) * wy1 + 32768) >> 16;
            drow[i] = ol_display_rgb(ctx, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
}

/* Raw back->framebuffer copy of a clamped rect. The parallel present workers
 * (one per CPU) each call this on a disjoint band of rows -- disjoint writes,
 * read-only shared source, so no locking is needed. */

void ol_display_clear(struct ol_display *ctx, uint32_t color)
{
    struct ol_pixel_target *s = &ctx->target;
    ol_display_fill_rect(ctx, 0, 0, s->w, s->h, color);
}

void ol_display_fill_rect(struct ol_display *ctx, int x, int y, int w, int h, uint32_t color)
{
    struct ol_pixel_target *s = &ctx->target;
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
    for (int j = j0; j < j1; j++) {
        uint32_t *row = s->px + (long)(y + j) * s->w + x;
        for (int i = i0; i < i1; i++) row[i] = color;
    }
}

/* How much of local point (i,j) is covered by a rounded rect of size w x h with
 * corner `rad`? 0..255, where the old inside_round() returned 0 or 1.
 *
 * WHY THIS CHANGED (the staircase). The test used to be `dx*dx + dy*dy <=
 * rad*rad` -- a point sample, so a corner came out as a staircase and a circle
 * came out as a square with a nub on each axis. aui.h:34-43 names it: "that
 * staircase is the single loudest thing that dates the UI", and it is on
 * screen constantly -- the three traffic lights, the dark-mode knob, the
 * dock's slab, every window frame corner, and (through fb_blur_rect's
 * `corner`) the edge of every glass panel.
 *
 * WHY IT ASKS OPEN LOGIT RATHER THAN COMPUTING COVERAGE HERE. A fifth coverage
 * rasterizer in this file is exactly the mistake c/lib/gfx was built to end --
 * it deleted two of them on the way in. The corner tile is generated by the
 * same scanline rasterizer the toolkit and the browser draw with, is cached
 * by exact device geometry, and is checked against a 16x supersampled
 * analytic oracle (worst pixel error 0.091) plus a second, independently
 * written oracle in test-aui-mask. Nothing here has to be trusted on its own.
 *
 * WHY THIS IS TWO FUNCTIONS AND NOT ONE (the split that replaced the old
 * single cover_round()). shadow_tile(ctx, ) below has always fetched its mask ONCE
 * per shape from its caller and then indexed straight into it per pixel --
 * that is the cheap, correct shape. cover_round() did not follow its own
 * neighbour's pattern: it called gfx_mask_corner() -- a linear scan of a
 * 16-slot cache -- from INSIDE the per-pixel loop, so one 40x40 button ran
 * the cache lookup ~1,600 times for a shape that needs exactly one. Splitting
 * "get the tile" (corner_mask_for, called once per shape, before the loop)
 * from "read one pixel of it" (corner_cov, called once per pixel, a straight
 * array index) is what makes every caller below match shadow_tile exactly.
 * The coverage values, the mask cache, and the fallback rule are UNCHANGED --
 * this is a call-site restructuring, not a new computation. */
static const unsigned char *corner_mask_for(int w, int h, int *rad)
{
    int r = *rad;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { *rad = 0; return 0; }
    *rad = r;
    return gfx_mask_corner(GFX_MASK_FILL, r, r, 0);
}

/* Coverage of local point (i,j) against the tile `m` that corner_mask_for()
 * already fetched for this shape, at the ALREADY-CLAMPED radius `rad` it
 * already wrote back -- callers must pass that one, not the radius they
 * originally asked for, or the clamp and the tile disagree on where the
 * straight bands end. */
static int corner_cov(int i, int j, int w, int h, int rad, const unsigned char *m)
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

/* Blend `color` over the target at (x,y) by coverage `a` (0..255). The three
 * hand-written copies of this arithmetic that used to sit in the loops below
 * are now one place, which is also the only place the rounding is decided. */
static void blend_cov(struct ol_display *ctx, int x, int y, uint32_t color, int a)
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

/* A circle is a rounded rect whose corner radius is half its side, so this is
 * the same coverage the corners use and the two cannot drift apart. It matters
 * here more than anywhere: the traffic lights are 12 px across, and at that
 * size the old `i*i + j*j <= r*r` did not read as a circle at all -- it read as
 * a square with one pixel poking out on each axis. */
void ol_display_fill_circle(struct ol_display *ctx, int cx, int cy, int r, uint32_t color)
{
    int d = 2 * r + 1;
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, cx - r, cy - r, d, d, &i0, &j0, &i1, &j1)) return;
    int rad = r;
    const unsigned char *m = corner_mask_for(d, d, &rad);
    for (int j = j0; j < j1; j++)
        for (int i = i0; i < i1; i++)
            blend_cov(ctx, cx - r + i, cy - r + j, color, corner_cov(i, j, d, d, rad, m));
}

void ol_display_round_rect(struct ol_display *ctx, int x, int y, int w, int h, int radius, uint32_t color)
{
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
    int rad = radius;
    const unsigned char *m = corner_mask_for(w, h, &rad);
    for (int j = j0; j < j1; j++)
        for (int i = i0; i < i1; i++)
            blend_cov(ctx, x + i, y + j, color, corner_cov(i, j, w, h, rad, m));
}

/* Blit an 8-bit coverage bitmap as anti-aliased text: each cov[i] is the alpha
 * of `color` over the existing pixel.
 *
 * THE ROW POINTER IS THE POINT. clip_ij has already proved every (i,j) in the
 * ranges it returned is inside the surface AND inside the clip, so the three
 * fb_get/fb_put calls per pixel were re-deriving the target, re-checking two
 * bounds and re-checking the scissor for a pixel already known to be writable
 * -- the same shape fb_blit_surface's header describes, one layer down, on the
 * busiest per-pixel loop the kernel still has. Output is unchanged: fb_get
 * inside the clip returns the pixel, fb_put inside the clip stores it. */
void ol_display_blit_glyph(struct ol_display *ctx, int x, int y, const uint8_t *cov, int w, int h, uint32_t color)
{
    int cr, cg, cb; unpack(ctx, color, &cr, &cg, &cb);
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
    struct ol_pixel_target *s = &ctx->target;
    for (int j = j0; j < j1; j++) {
        const uint8_t *crow = cov + (long)j * w;
        uint32_t *drow = s->px + (long)(y + j) * s->w + x;
        for (int i = i0; i < i1; i++) {
            int a = crow[i];
            if (!a) continue;
            if (a >= 255) { drow[i] = color; continue; }
            int br, bg, bb; unpack(ctx, drow[i], &br, &bg, &bb);
            int nr = (cr * a + br * (255 - a)) / 255;
            int ng = (cg * a + bg * (255 - a)) / 255;
            int nb = (cb * a + bb * (255 - a)) / 255;
            drow[i] = ol_display_rgb(ctx, (uint8_t)nr, (uint8_t)ng, (uint8_t)nb);
        }
    }
}

void ol_display_blend_rect(struct ol_display *ctx, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
    struct ol_pixel_target *s = &ctx->target;      /* clip_ij already proved every (i,j) writable */
    for (int j = j0; j < j1; j++) {
        uint32_t *drow = s->px + (long)(y + j) * s->w + x;
        for (int i = i0; i < i1; i++) {
            int br, bg, bb;
            unpack(ctx, drow[i], &br, &bg, &bb);
            int nr = (r * a + br * (255 - a)) / 255;
            int ng = (g * a + bg * (255 - a)) / 255;
            int nb = (b * a + bb * (255 - a)) / 255;
            drow[i] = ol_display_rgb(ctx, (uint8_t)nr, (uint8_t)ng, (uint8_t)nb);
        }
    }
}

void ol_display_blend_round_rect(struct ol_display *ctx, int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
    int rad = radius;
    const unsigned char *m = corner_mask_for(w, h, &rad);
    for (int j = j0; j < j1; j++) {
        for (int i = i0; i < i1; i++) {
            /* Two alphas meet here and they MULTIPLY: the caller's opacity and
             * the corner's coverage. Taking either one alone gives a panel that
             * is translucent in the middle and hard-edged at the corner, which
             * is what the boolean test used to produce. */
            int cov = corner_cov(i, j, w, h, rad, m);
            if (cov <= 0) continue;
            blend_cov(ctx, x + i, y + j, ol_display_rgb(ctx, r, g, b), a * cov / 255);
        }
    }
}

/* Blit one shadow corner tile, optionally mirrored, so ONE rasterized quadrant
 * serves all four corners. */
static void shadow_tile(struct ol_display *ctx, int x, int y, int T, const unsigned char *m,
                        int alpha, int flipx, int flipy)
{
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, T, T, &i0, &j0, &i1, &j1)) return;
    for (int j = j0; j < j1; j++) {
        int sj = flipy ? T - 1 - j : j;
        for (int i = i0; i < i1; i++) {
            int si = flipx ? T - 1 - i : i;
            blend_cov(ctx, x + i, y + j, ol_display_rgb(ctx, 0, 0, 0), m[(long)sj * T + si] * alpha / 255);
        }
    }
}

/* A drop shadow around a rounded rect: offset `dy` down, falling off over
 * `blur`, peak opacity `alpha`.
 *
 * WHAT THIS REPLACES. wm.c drew window shadows as three nested constant-alpha
 * rectangles -- thicknesses 8/4/2 at alphas 11/22/40 -- which is a shadow with
 * TWO defects, and the second is the one you actually see. The alpha was a
 * three-step staircase instead of a falloff, and the bands were SQUARE around a
 * window whose corners are rounded, so each corner carried a dark nub of shadow
 * sitting outside a corner that curves away from it. It was the band down the
 * left of every window frame in every screenshot in this tree.
 *
 * WHY IT IS NOT A ROUNDED-RECT BLEND. The comment on the old code was right
 * about the cost and is the constraint here: the window is opaque and overdraws
 * its own interior, so blending a whole window-sized shape is ~30x wasted work
 * on every repaint, and a big window lagged on each flush. So this paints the
 * PERIMETER ONLY, in the shape aui_shadow_ex already uses in ring 3: four
 * corner tiles from the engine's cache, four edge strips, and one flat band
 * closing the sliver the offset opens under the caster.
 *
 * It is also CHEAPER than what it replaces. The old bands touched perimeter*14
 * pixels (8+4+2). This touches perimeter*blur plus four (blur+radius)^2 tiles,
 * and the tiles are cached across frames -- at blur=8 that is perimeter*8 plus
 * about 1,300 pixels, against perimeter*14, on a 640x480 window 19k against
 * 31k. Each edge strip is one constant-alpha fb_blend_rect per row, because
 * along an edge the falloff depends only on distance.
 *
 * The corners and the edges MUST fall off by the same curve or the joins show
 * as seams, which is exactly why gfx.h exposes gfx_shadow_falloff next to
 * gfx_corner_shadow -- the tile calls it per pixel, the strips call it per row,
 * and the sample points are matched (pixel centres, distance measured from the
 * caster's edge). */
/* How many ol_display_shadow(ctx, ) calls this boot had to shrink `blur` below what was
 * asked for, to dodge gfx_mask_corner's refusal instead of hitting it -- see
 * the comment on the clamp below. THIS is the one call site in the tree that
 * AVOIDS the refusal rather than handling it, which sounds strictly better
 * until you notice the shadow it draws is quietly not the shadow that was
 * requested, and nothing said so: unlike every other site fixed this
 * milestone (all of which go through gfx_mask_corner and are covered by ITS
 * refusal counter, gfx_mask.c's mrefuse), a pre-clamp changes the request
 * BEFORE gfx_mask_corner ever sees it, so that counter never fires here.
 * `fb_shadow_clamped` is this call site's own count, for exactly that gap --
 * kernel .bss cost: one unsigned int, the cheapest fix available for a
 * function that (correctly, per gfx.h's LIMITS section) cannot afford
 * aui.c's BIG_MASK buffers to avoid the clamp altogether. */

unsigned ol_display_shadow_clamp_count(struct ol_display *ctx) { return fb_shadow_clamped; }

void ol_display_shadow(struct ol_display *ctx, int x, int y, int w, int h, int radius, int dy, int blur, uint8_t alpha)
{
    if (w <= 0 || h <= 0 || blur <= 0 || alpha == 0) return;
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    /* Clamp the blur rather than drop the corners. gfx_mask_corner refuses a
     * tile past GFX_MASK_MAX and returns NULL, and losing the corners is far
     * more visible than a slightly tighter shadow -- it is the square-nub bug
     * this function exists to remove, reintroduced at high display scales. */
    if (blur + radius > GFX_MASK_MAX) { fb_shadow_clamped++; blur = GFX_MASK_MAX - radius; }
    if (blur <= 0) return;

    int sy = y + dy, T = blur + radius;
    const unsigned char *m = gfx_mask_corner(GFX_MASK_SHADOW, T, T, radius);
    if (m) {
        shadow_tile(ctx, x - blur,        sy - blur,          T, m, alpha, 0, 0);
        shadow_tile(ctx, x + w - radius,  sy - blur,          T, m, alpha, 1, 0);
        shadow_tile(ctx, x - blur,        sy + h - radius,    T, m, alpha, 0, 1);
        shadow_tile(ctx, x + w - radius,  sy + h - radius,    T, m, alpha, 1, 1);
    }

    /* The offset exposes a sliver of the shadow box's interior below the
     * caster, and the slices above deliberately do not paint any interior.
     * Left out, every window shows a dy-pixel gap of clean background between
     * itself and its own shadow -- which is what a shadow never does. */
    if (dy > 0 && w > 2 * radius)
        ol_display_blend_rect(ctx, x + radius, y + h, w - 2 * radius, dy, 0, 0, 0, alpha);
    /* When the offset exceeds the corner radius the same exposure reaches the
     * CORNER columns: the bottom tiles do not begin until sy + h - radius,
     * which with dy > radius sits BELOW the caster's bottom edge, and nothing
     * else touches the two radius-wide spans under the corners -- the sliver
     * above covers only the straight middle, the side strips only columns
     * outside the box. Found as four rows of bright wallpaper punched out of
     * the shadow at each bottom corner, the first time dy (14pt) grew past
     * the corner radius (10pt); every earlier tuning had dy <= radius, which
     * makes these rects zero-height, which is why the gap was never seen.
     * Full alpha is correct here for the same reason it is in the sliver:
     * these rows are interior to the offset shadow's body, above where the
     * corner curve begins. */
    if (dy > radius) {
        ol_display_blend_rect(ctx, x,              y + h, radius, dy - radius, 0, 0, 0, alpha);
        ol_display_blend_rect(ctx, x + w - radius, y + h, radius, dy - radius, 0, 0, 0, alpha);
    }

    long blur256 = (long)blur * 256;
    for (int e = 0; e < blur; e++) {
        int a = gfx_shadow_falloff((long)e * 256 + 128, blur256) * alpha / 255;
        if (a <= 0) continue;
        if (w > 2 * radius) {
            ol_display_blend_rect(ctx, x + radius, sy - 1 - e, w - 2 * radius, 1, 0, 0, 0, (uint8_t)a);
            ol_display_blend_rect(ctx, x + radius, sy + h + e, w - 2 * radius, 1, 0, 0, 0, (uint8_t)a);
        }
        if (h > 2 * radius) {
            ol_display_blend_rect(ctx, x - 1 - e, sy + radius, 1, h - 2 * radius, 0, 0, 0, (uint8_t)a);
            ol_display_blend_rect(ctx, x + w + e, sy + radius, 1, h - 2 * radius, 0, 0, 0, (uint8_t)a);
        }
    }
}

/* Linear interpolate two packed colors: a*(den-num)/den + b*num/den. */
static uint32_t color_lerp(struct ol_display *ctx, uint32_t a, uint32_t b, int num, int den)
{
    int ar, ag, ab, br, bg, bb;
    unpack(ctx, a, &ar, &ag, &ab);
    unpack(ctx, b, &br, &bg, &bb);
    int r = ar + (br - ar) * num / den;
    int g = ag + (bg - ag) * num / den;
    int bl = ab + (bb - ab) * num / den;
    return ol_display_rgb(ctx, (uint8_t)r, (uint8_t)g, (uint8_t)bl);
}

/* Lighten (delta>0) or darken (delta<0) a packed color by delta per channel. */
uint32_t ol_display_shade(struct ol_display *ctx, uint32_t c, int delta)
{
    int r, g, b;
    unpack(ctx, c, &r, &g, &b);
    r += delta; g += delta; b += delta;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ol_display_rgb(ctx, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* Vertical gradient fill: row j gets lerp(top..bottom). Integer, one lerp/row. */
void ol_display_fill_vgrad(struct ol_display *ctx, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
    struct ol_pixel_target *s = &ctx->target;
    for (int j = j0; j < j1; j++) {
        uint32_t c = color_lerp(ctx, top, bottom, j, h > 1 ? h - 1 : 1);
        uint32_t *row = s->px + (long)(y + j) * s->w + x;
        for (int i = i0; i < i1; i++) row[i] = c;
    }
}

/* Rounded-rect vertical gradient (corners antialiased by corner_cov). */
void ol_display_round_rect_vgrad(struct ol_display *ctx, int x, int y, int w, int h, int radius, uint32_t top, uint32_t bottom)
{
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
    int rad = radius;
    const unsigned char *m = corner_mask_for(w, h, &rad);
    for (int j = j0; j < j1; j++) {
        uint32_t c = color_lerp(ctx, top, bottom, j, h > 1 ? h - 1 : 1);
        for (int i = i0; i < i1; i++)
            blend_cov(ctx, x + i, y + j, c, corner_cov(i, j, w, h, rad, m));
    }
}

/* Real-time backdrop blur of a rect on the current target -- the basis for
 * "vibrancy" (frost the live content behind a translucent panel). A separable
 * box blur: a horizontal moving-sum pass into a scratch buffer, then a vertical
 * moving-sum pass back into the target. Both passes are O(1) per pixel (the
 * window sum slides), so cost is O(w*h), radius-independent. `corner` > 0 rounds
 * the written region (corner pixels keep their pre-blur value), so a rounded
 * panel doesn't leave blurred nubs outside its corners. Integer-only. */


void ol_display_blur_rect(struct ol_display *ctx, int x, int y, int w, int h, int radius, int corner)
{
    struct ol_pixel_target *s = &ctx->target;
    if (!s->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0 || radius < 1) return;
    if (w * h > blur_scratch_n) {
        if (blur_scratch) display_free(ctx, blur_scratch);
        blur_scratch = (uint32_t *)display_alloc(ctx, (unsigned long)w * h * 4);
        blur_scratch_n = blur_scratch ? w * h : 0;
    }
    if (!blur_scratch) return;
    uint32_t *tmp = blur_scratch;

    for (int j = 0; j < h; j++) {                 /* horizontal: target row -> tmp */
        const uint32_t *srow = s->px + (long)(y + j) * s->w + x;
        uint32_t *trow = tmp + (long)j * w;
        int sr = 0, sg = 0, sb = 0, cnt = 0, r, g, b;
        for (int k = 0; k <= radius && k < w; k++) { unpack(ctx, srow[k], &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
        for (int i = 0; i < w; i++) {
            trow[i] = ol_display_rgb(ctx, (uint8_t)(sr / cnt), (uint8_t)(sg / cnt), (uint8_t)(sb / cnt));
            int a = i + radius + 1; if (a < w)  { unpack(ctx, srow[a],  &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
            int d = i - radius;     if (d >= 0) { unpack(ctx, srow[d],  &r, &g, &b); sr -= r; sg -= g; sb -= b; cnt--; }
        }
    }
    /* Fetched once for the whole rect, not once per pixel -- see the header
     * comment on corner_mask_for()/corner_cov() earlier in this file. `corner`
     * is constant for this call, so hoisting it out of the double loop below
     * is exactly the shadow_tile(ctx, ) pattern, not a behaviour change: `crad` is
     * `corner`, clamped to this rect's own w/2,h/2 the same way it always was. */
    int crad = corner;
    const unsigned char *cm = corner_mask_for(w, h, &crad);
    for (int i = 0; i < w; i++) {                  /* vertical: tmp column -> target */
        int sr = 0, sg = 0, sb = 0, cnt = 0, r, g, b;
        for (int k = 0; k <= radius && k < h; k++) { unpack(ctx, tmp[(long)k * w + i], &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
        for (int j = 0; j < h; j++) {
            /* The clip bounds the WRITE only. This primitive reads a
             * neighbourhood, so it cannot simply be run over a smaller
             * rectangle: the pixels it samples outside the clip are still
             * needed. wm.c's damage tracking therefore never hands it a
             * partially-clipped panel -- see the glass-panel expansion there --
             * and this check is the belt to that braces. */
            /* The corner is a COVERAGE, not a yes/no. A glass panel's edge is
             * the most visible curve on the machine -- it is the dock, the menu
             * bar and every titlebar -- and a boolean here left it stepped no
             * matter how smooth the blur inside it was. Blending the blurred
             * value against the pixel's own pre-blur colour is what makes the
             * edge fade out instead of ending. */
            int cov = corner_cov(i, j, w, h, crad, cm);
            if (cov > 0 && clip_px(s, x + i, y + j)) {
                uint32_t bl = ol_display_rgb(ctx, (uint8_t)(sr / cnt), (uint8_t)(sg / cnt), (uint8_t)(sb / cnt));
                uint32_t *px = &s->px[(long)(y + j) * s->w + (x + i)];
                if (cov >= 255) {
                    *px = bl;
                } else {
                    int nr, ng, nb, orr, og, ob;
                    unpack(ctx, bl, &nr, &ng, &nb);
                    unpack(ctx, *px, &orr, &og, &ob);
                    *px = ol_display_rgb(ctx, (uint8_t)((nr * cov + orr * (255 - cov)) / 255),
                                 (uint8_t)((ng * cov + og  * (255 - cov)) / 255),
                                 (uint8_t)((nb * cov + ob  * (255 - cov)) / 255));
                }
            }
            int a = j + radius + 1; if (a < h)  { unpack(ctx, tmp[(long)a * w + i], &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
            int d = j - radius;     if (d >= 0) { unpack(ctx, tmp[(long)d * w + i], &r, &g, &b); sr -= r; sg -= g; sb -= b; cnt--; }
        }
    }
}

/* "Liquid Glass": frost the live backdrop of a rounded-rect panel AND refract it
 * through the curved rim, with a specular rim highlight + body tint -- Apple's
 * Liquid Glass material, integer-only (modelled from a Python optics study,
 * /tmp/glass_study.py). Pipeline: copy the backdrop -> separable box blur -> for
 * each pixel compute the rounded-rect signed distance + outward normal; within an
 * edge band sample the blurred backdrop displaced INWARD along the normal (the
 * lens/bevel that bends the background at the rim); tint; add a rim highlight
 * where the bevel faces the (up-left) light and a faint contact shadow opposite.
 * Pixels outside the rounded rect are left untouched (so a drop shadow shows). */




void ol_display_liquid_glass(struct ol_display *ctx, int x, int y, int w, int h, int radius,
                     uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    ol_display_liquid_glass_cut(ctx, x, y, w, h, radius, tr, tg, tb, ta, 0);
}

/* EXACT integer reciprocals for the box blur's window count.
 *
 * The moving sum's divisor is the number of samples currently in the window --
 * 2*RB+1 in the middle of a row and fewer at the two ends -- so it is a RUNTIME
 * divisor, and clang cannot turn it into a multiply the way it already does for
 * every /255 and /256 in this file. On x86_64 that leaves an `idivl`, and under
 * TCG an idivl is a HELPER CALL rather than an instruction. There are six of
 * them per pixel over the WHOLE panel (three channels x two passes) and they
 * ran before any clip was consulted, which made them the single largest item in
 * the glass.
 *
 * (s * blur_rcp[c]) >> 16 == s / c for every c in 1..2*RB+1 and every s that is
 * a sum of at most c bytes. That is VERIFIED, not argued: all 3,315 reachable
 * (c, s) pairs were checked against the division. The largest product is
 * 16,714,230, so the multiply cannot overflow an int. Same family as g_acc's
 * 16.16 reciprocal in c/lib/gfx -- and the same reason for existing: a divide
 * in the innermost loop of a coverage pass. */
#define GLASS_RB 6
static const int blur_rcp[2 * GLASS_RB + 2] = {
    0, 65536, 32768, 21846, 16384, 13108, 10923, 9363,
    8192, 7282, 6554, 5958, 5462, 5042
};
#define BDIV(s, c) (((s) * blur_rcp[(c)]) >> 16)

/* The per-pixel tail of the glass, shared by the general path and by the
 * row-constant fast path below so the two cannot drift. Order is load-bearing:
 * tint, then the environment reflection, then the specular wash, then the
 * contact shadow -- each blend is against the RESULT of the previous one. */
static inline void glass_shade(int *pr, int *pg, int *pb,
                               int tr, int tg, int tb, int ta,
                               int env, int fr, int hi, int sh)
{
    int r = *pr, gg = *pg, b = *pb;
    r += (tr - r) * ta / 255; gg += (tg - gg) * ta / 255; b += (tb - b) * ta / 255;
    r += (env - r) * fr / 255; gg += (env - gg) * fr / 255; b += (env - b) * fr / 255;
    r += (255 - r) * hi / 256; gg += (255 - gg) * hi / 256; b += (255 - b) * hi / 256;
    r -= r * sh / 256; gg -= gg * sh / 256; b -= b * sh / 256;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (gg < 0) gg = 0; if (gg > 255) gg = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    *pr = r; *pg = gg; *pb = b;
}

/* The edge pixel is part panel, part whatever was behind it -- and `behind it`
 * is still in the target, because the loop reads from the saved copy and writes
 * here. */
static inline void glass_store(struct ol_display *ctx, uint32_t *dstp, int r, int gg, int b, int gcov)
{
    if (gcov >= 255) { *dstp = ol_display_rgb(ctx, (uint8_t)r, (uint8_t)gg, (uint8_t)b); return; }
    int orr, og, ob; unpack(ctx, *dstp, &orr, &og, &ob);
    *dstp = ol_display_rgb(ctx, (uint8_t)((r  * gcov + orr * (255 - gcov)) / 255),
                   (uint8_t)((gg * gcov + og  * (255 - gcov)) / 255),
                   (uint8_t)((b  * gcov + ob  * (255 - gcov)) / 255));
}

void ol_display_liquid_glass_cut(struct ol_display *ctx, int x, int y, int w, int h, int radius,
                         uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta,
                         unsigned cut)
{
    struct ol_pixel_target *s = &ctx->target;
    if (!s->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0 || radius < 1) return;
    /* THE WRITE RANGE, UP FRONT.
     *
     * Everything below -- the backdrop copy, both blur passes, the SDF, the two
     * isqrts, the coverage -- used to run over the whole panel unconditionally,
     * and the clip was consulted per pixel just before the store. So a frame
     * whose damage rectangle is over the dock still re-frosted the browser's
     * entire 101,340-pixel titlebar and then threw every pixel of it away.
     *
     * A PARTIAL intersection still cannot be narrowed: the blur reads a
     * neighbourhood, so the pixels it samples outside the clip are needed
     * (that is the note in fb_blur_rect, and why wm.c grows a damage rectangle
     * to contain a whole glass panel). A ZERO intersection can -- there is no
     * pixel this call is allowed to write, and it has no other effect, so
     * returning here is exactly what the old code computed.
     *
     * The surviving range also REPLACES the per-pixel clip_px() in the field
     * loop: x/y/w/h are already clamped to the surface above, so for i in
     * [ci0,ci1) and j in [cj0,cj1) the test clip_px() performed is true by
     * construction. Same pixels, and the rows and columns it would have
     * rejected are never visited. */
    int ci0 = 0, cj0 = 0, ci1 = w, cj1 = h;
    if (s->clip_on) {
        if (s->clx0 - x > ci0) ci0 = s->clx0 - x;
        if (s->cly0 - y > cj0) cj0 = s->cly0 - y;
        if (s->clx1 - x < ci1) ci1 = s->clx1 - x;
        if (s->cly1 - y < cj1) cj1 = s->cly1 - y;
        if (ci0 >= ci1 || cj0 >= cj1) return;
    }
    if (w * h > glass_buf_n) { if (glass_buf) display_free(ctx, glass_buf); glass_buf = (uint32_t *)display_alloc(ctx, (unsigned long)w * h * 4); glass_buf_n = glass_buf ? w * h : 0; }
    int lmax = w > h ? w : h;
    /* One lane, for the per-column geometry. This used to be THREE, holding a
     * blurred row/column as separate R, G and B ints between each moving-sum
     * pass and the pack that wrote it back; both passes now pack as they go
     * (see below), so the transposed line is gone and with it 24 KiB at
     * 1920 wide. */
    if (lmax > glass_line_n) { if (glass_line) display_free(ctx, glass_line); glass_line = (int *)display_alloc(ctx, (unsigned long)lmax * sizeof(int)); glass_line_n = glass_line ? lmax : 0; }
    if (!glass_buf || !glass_line) { ol_display_blur_rect(ctx, x, y, w, h, 6, radius); return; }   /* frost-only fallback */
    uint32_t *g = glass_buf;

    /* FOUR PASSES OVER THE PANEL BECAME TWO. The backdrop copy existed only so
     * the horizontal pass had somewhere to read from -- but the horizontal pass
     * WRITES somewhere else (the scratch), so it can read the live backdrop
     * directly and the copy is the same loads a second time. The write-back
     * pass existed because the moving sum's subtract re-reads a pixel the pack
     * would have overwritten; along a row that pixel is `i - RB`, six columns
     * BEHIND, so reading the untouched source keeps it. Down a column the
     * source IS the destination, so the vertical pass keeps the last RB+1
     * originals in a ring instead of a whole line.
     * Bit-identical: the value stored is ol_display_rgb(ctx, ) of exactly the three sums the
     * line buffer used to carry. */
    const int RB = GLASS_RB;                 /* window count <= 2*RB+1 -- blur_rcp's domain */
    for (int j = 0; j < h; j++) {            /* blur: horizontal moving sum, backdrop -> scratch */
        const uint32_t *srow = s->px + (long)(y + j) * s->w + x;
        uint32_t *drow = g + (long)j * w;
        int sr = 0, sg = 0, sb = 0, cnt = 0, r, gg, b;
        for (int k = 0; k <= RB && k < w; k++) { unpack(ctx, srow[k], &r, &gg, &b); sr += r; sg += gg; sb += b; cnt++; }
        for (int i = 0; i < w; i++) {
            drow[i] = ol_display_rgb(ctx, (uint8_t)BDIV(sr, cnt), (uint8_t)BDIV(sg, cnt), (uint8_t)BDIV(sb, cnt));
            int a = i + RB + 1; if (a < w)  { unpack(ctx, srow[a], &r, &gg, &b); sr += r; sg += gg; sb += b; cnt++; }
            int d = i - RB;     if (d >= 0) { unpack(ctx, srow[d], &r, &gg, &b); sr -= r; sg -= gg; sb -= b; cnt--; }
        }
    }
    for (int i = 0; i < w; i++) {            /* blur: vertical moving sum, in place */
        int sr = 0, sg = 0, sb = 0, cnt = 0, r, gg, b;
        uint32_t ring[GLASS_RB + 1];         /* the last RB+1 pre-blur values */
        int rp = 0;                          /* == j % (RB+1) */
        for (int k = 0; k <= RB && k < h; k++) { unpack(ctx, g[(long)k * w + i], &r, &gg, &b); sr += r; sg += gg; sb += b; cnt++; }
        for (int j = 0; j < h; j++) {
            uint32_t *cell = &g[(long)j * w + i];
            ring[rp] = *cell;
            *cell = ol_display_rgb(ctx, (uint8_t)BDIV(sr, cnt), (uint8_t)BDIV(sg, cnt), (uint8_t)BDIV(sb, cnt));
            int a = j + RB + 1; if (a < h)  { unpack(ctx, g[(long)a * w + i], &r, &gg, &b); sr += r; sg += gg; sb += b; cnt++; }
            /* (j - RB) % (RB+1) == (rp + 1) % (RB+1) */
            int d = j - RB;     if (d >= 0) { unpack(ctx, ring[rp == RB ? 0 : rp + 1], &r, &gg, &b); sr -= r; sg -= gg; sb -= b; cnt--; }
            rp = rp == RB ? 0 : rp + 1;
        }
    }

    int cx = w / 2, cy = h / 2, ix = cx - radius, iy = cy - radius;
    int minside = w < h ? w : h;
    int E = 22; if (E > minside / 2) E = minside / 2; if (E < 4) E = 4;   /* thin panels -> smaller band */
    int REFRACT = 18; if (REFRACT > E) REFRACT = E;
    glass_build_lut(E, REFRACT);           /* cached on (E, REFRACT); see above */
    int ELUT = E > GLASS_E_MAX ? GLASS_E_MAX : E;
    const int SPEC = 64;                   /* was 150, before the rim existed */
    int eband = E * 7 / 10; if (eband < 1) eband = 1;

    /* THE FIELD BELOW IS PURE GEOMETRY, and the panel's geometry does not change
     * between frames. glass_build_lut() already caches the part that depends only
     * on (E, REFRACT); these are the three parts that depend on the pixel, and
     * each is hoisted to the axis it actually belongs to rather than cached per
     * pixel -- so there is no field to invalidate and no stale picture to draw.
     *
     *  1. `band` and `tilt` divide by eband and E. Both divisors are constant
     *     for the whole panel but neither is a compile-time constant, so clang
     *     leaves an idivl -- a helper call under TCG -- in the innermost loop.
     *     Both terms are ZERO past their band (band past eband, tilt past E, and
     *     eband = E*7/10 <= E), so E entries cover every value either can take.
     *  2. The cut folding and the horizontal half-distance depend on the COLUMN
     *     only. colq[] is that, computed once per panel.
     *  3. The vertical half-distance depends on the ROW only, hoisted out of the
     *     i loop. */
    int bandt[GLASS_E_MAX + 1], tiltt[GLASS_E_MAX + 1];
    for (int d = 0; d < E; d++) {
        int bd = 256 - d * 256 / eband; if (bd < 0) bd = 0;
        bandt[d] = bd;
        tiltt[d] = (E - d) * 256 / E;
    }
    int *colq = glass_line;                /* w ints; the blur no longer uses it */
    for (int i = 0; i < w; i++) {
        int pxv = i - cx, axv = pxv < 0 ? -pxv : pxv;
        /* A CUT edge is not an edge (see fb.h): folding its half-axis to 0
         * puts every pixel on that side "deep inside" as far as the SDF, the
         * rim band and the Fresnel term are concerned, so the bevel machinery
         * below never fires there. The frost and tint above are untouched -- a
         * cut panel is still glass, it just has no rim. */
        if ((cut & OL_GLASS_CUT_LEFT)  && pxv < 0) axv = 0;
        if ((cut & OL_GLASS_CUT_RIGHT) && pxv > 0) axv = 0;
        colq[i] = axv - ix;
    }
    /* colq is non-increasing on [0,cx] and non-decreasing on [cx,w) -- an
     * absolute value, and a cut only flattens one arm of it -- so for any
     * threshold the set { i : colq[i] <= t } is a contiguous run. That is what
     * makes the fast path below a range rather than a per-pixel test. */

    for (int j = cj0; j < cj1; j++) {
        int py = j - cy, ay = py < 0 ? -py : py;
        if ((cut & OL_GLASS_CUT_TOP)    && py < 0) ay = 0;
        if ((cut & OL_GLASS_CUT_BOTTOM) && py > 0) ay = 0;
        int qy = ay - iy, qyc = qy > 0 ? qy : 0;
        int sy = py < 0 ? -1 : 1;

        /* THE ROW-DOMINANT RUN. Where the column contributes nothing to the
         * distance field -- qxc == 0 AND the row is at least as close to its
         * edge as the column is to its own -- every term below is a function of
         * j alone: outd is qyc*256, ins is qy, the normal is (0, +-256), and so
         * gcov, depth, the three displacements, env, fr, hi and sh are all
         * constant across the run. Only the three backdrop samples vary, and
         * because nx is 0 they vary only in i, along three fixed rows of the
         * blurred copy. On the dock that is ~90% of the panel.
         *
         * The threshold is one expression for both cases: with qyc > 0 the row
         * is already inside the rim band and any column with qx <= 0 qualifies;
         * with qyc == 0 (so qy <= 0) the column must additionally not be nearer
         * its edge than the row, i.e. qx <= qy -- which subsumes qx <= 0.
         * `qx > qy` is the branch the general path takes, so <= keeps the
         * boundary column on the same side it was always on. */
        int run0 = ci1, run1 = ci1;            /* empty unless the search finds a run */
#ifndef GLASS_FIELD_SLOW
        {
            int thr = qyc > 0 ? 0 : qy;
            if (colq[cx] <= thr) {
                int lo, hi2;
                int a2 = 0, b2 = cx;               /* non-increasing: first <= thr */
                while (a2 < b2) { int m = (a2 + b2) / 2; if (colq[m] <= thr) b2 = m; else a2 = m + 1; }
                lo = a2;
                a2 = cx; b2 = w - 1;               /* non-decreasing: last <= thr */
                while (a2 < b2) { int m = (a2 + b2 + 1) / 2; if (colq[m] <= thr) a2 = m; else b2 = m - 1; }
                hi2 = a2;
                run0 = lo > ci0 ? lo : ci0;
                run1 = hi2 + 1 < ci1 ? hi2 + 1 : ci1;
                if (run1 < run0) run1 = run0;
            }
        }
#endif
        if (run1 > run0) {
            /* One evaluation of the field for the whole run. */
            int outd = qyc ? (int)gl_isqrt((unsigned long)qyc * qyc << 16) : 0;
            int ins = qy > 0 ? 0 : qy;             /* qx <= qy here, and clamped */
            int sdf = outd + (ins - radius) * 256;
            int gcov = 128 - sdf;
            if (gcov <= 0) { run1 = run0; }            /* outside: the run draws nothing */
            else {
                if (gcov > 255) gcov = 255;
                int depth = -sdf / 256; if (depth < 0) depth = 0;
                int ny = sy * 256;                 /* nx is 0: the normal is vertical */
                int di = depth < ELUT ? depth : ELUT;
                int yr = j - sy * glass_disp[0][di];
                int yg = j - sy * glass_disp[1][di];
                int yb = j - sy * glass_disp[2][di];
                if (yr < 0) yr = 0; if (yr >= h) yr = h - 1;
                if (yg < 0) yg = 0; if (yg >= h) yg = h - 1;
                if (yb < 0) yb = 0; if (yb >= h) yb = h - 1;
                int band = depth < E ? bandt[depth] : 0;
                int tilt = depth < E ? tiltt[depth] : 0;
                int facing = (ny * (-205)) / 256; if (facing < 0) facing = 0; if (facing > 256) facing = 256;
                int env = 176 + (facing - 128) * tilt / 256;
                if (env < 96)  env = 96;
                if (env > 250) env = 250;
                int fr = glass_fres[di];
                int hi = facing * band / 256; hi = hi * hi / 256; hi = hi * SPEC / 256;
                int op = (ny * 205) / 256; if (op < 0) op = 0;
                int sh = op * band / 256; sh = sh * sh / 256; sh = sh * 46 / 256;
                const uint32_t *rr = g + (long)yr * w, *rg = g + (long)yg * w, *rb = g + (long)yb * w;
                uint32_t *drow = &s->px[(long)(y + j) * s->w + x];
                if (rr == rg && rg == rb) {        /* no dispersion here: one fetch */
                    for (int i = run0; i < run1; i++) {
                        int r, gg, b; unpack(ctx, rr[i], &r, &gg, &b);
                        glass_shade(&r, &gg, &b, tr, tg, tb, ta, env, fr, hi, sh);
                        glass_store(ctx, &drow[i], r, gg, b, gcov);
                    }
                } else {
                    for (int i = run0; i < run1; i++) {
                        int r, gg, b, t1, t2;
                        unpack(ctx, rr[i], &r,  &t1, &t2);
                        unpack(ctx, rg[i], &t1, &gg, &t2);
                        unpack(ctx, rb[i], &t1, &t2, &b);
                        glass_shade(&r, &gg, &b, tr, tg, tb, ta, env, fr, hi, sh);
                        glass_store(ctx, &drow[i], r, gg, b, gcov);
                    }
                }
            }
        }

        /* Everything the run did not cover: the two ends of the row, which is
         * where the corner arcs and the vertical rim live. This is the general
         * path and it is what GLASS_FIELD_SLOW runs over the whole panel. */
        for (int i = ci0; i < ci1; i++) {
            if (i >= run0 && i < run1) { i = run1 - 1; continue; }
            int px = i - cx;
            int qx = colq[i], qxc = qx > 0 ? qx : 0;
            int sx = px < 0 ? -1 : 1;
            /* The distance is carried in 8.8 -- the square sum is shifted by 16
             * before the root, so isqrt returns 256*d. A whole-pixel SDF cannot
             * describe an edge that falls between two pixels, and this panel's
             * edge is the most looked-at curve on the machine. */
            unsigned long vv = (unsigned long)qxc * qxc + (unsigned long)qyc * qyc;
            int outd = vv ? (int)gl_isqrt(vv << 16) : 0;
            int ins = qx > qy ? qx : qy; if (ins > 0) ins = 0;
            int sdf = outd + (ins - radius) * 256;               /* 8.8, <0 inside */
            /* COVERAGE, not a yes/no. A pixel whose centre sits exactly on the
             * boundary is half covered, so the ramp is centred on sdf==0 and one
             * pixel wide. This is the whole reason the panel's edge stops being
             * a staircase: `if (sdf >= 0) continue` drew every edge pixel at
             * full strength and then stopped dead. */
            int gcov = 128 - sdf;
            if (gcov <= 0) continue;                             /* outside */
            if (gcov > 255) gcov = 255;
            int depth = -sdf / 256; if (depth < 0) depth = 0;
            int gx, gy, nlen;
            if (qxc > 0 || qyc > 0) {
                gx = sx * qxc; gy = sy * qyc;
                /* isqrt(v) is floor(sqrt(v)) exactly, so floor(256*sqrt(v))>>8 is
                 * floor(sqrt(v)) -- the same number outd already paid for. The
                 * shift is only valid while v<<16 fits gl_isqrt's 32-bit domain
                 * (b starts at 1<<30); past that outd is already saturated, so
                 * the second call is kept for a panel nobody draws. */
                nlen = vv < 65536UL ? (outd >> 8) : (int)gl_isqrt(vv);
                if (!nlen) nlen = 1;
            } else if (qx > qy) { gx = sx; gy = 0;  nlen = 1; }
            else                { gx = 0;  gy = sy; nlen = 1; }
            int nx, ny;                                          /* outward unit x256 */
            if (nlen == 1) { nx = gx * 256; ny = gy * 256; }
            else           { nx = gx * 256 / nlen; ny = gy * 256 / nlen; }
            /* One index instead of the old squared ramp, and three of them
             * because R, G and B leave the rim at different angles. Outside the
             * edge band all three are zero and the three samples collapse onto
             * the same pixel, so the interior costs what it always did. */
            int di = depth < ELUT ? depth : ELUT;
            int r, gg, b;
            {
                int dr = glass_disp[0][di], dg = glass_disp[1][di], db = glass_disp[2][di];
                int xr = i - nx * dr / 256, yr = j - ny * dr / 256;
                int xg = i - nx * dg / 256, yg = j - ny * dg / 256;
                int xb = i - nx * db / 256, yb = j - ny * db / 256;
                if (xr < 0) xr = 0; if (xr >= w) xr = w - 1;
                if (yr < 0) yr = 0; if (yr >= h) yr = h - 1;
                if (xg < 0) xg = 0; if (xg >= w) xg = w - 1;
                if (yg < 0) yg = 0; if (yg >= h) yg = h - 1;
                if (xb < 0) xb = 0; if (xb >= w) xb = w - 1;
                if (yb < 0) yb = 0; if (yb >= h) yb = h - 1;
                int t1, t2;
                unpack(ctx, g[(long)yr * w + xr], &r,  &t1, &t2);
                unpack(ctx, g[(long)yg * w + xg], &t1, &gg, &t2);
                unpack(ctx, g[(long)yb * w + xb], &t1, &t2, &b);
            }
            int band = depth < E ? bandt[depth] : 0;
            int facing = (nx * (-154) + ny * (-205)) / 256; if (facing < 0) facing = 0; if (facing > 256) facing = 256;
            /* THE EDGE. Reflectance goes to 1 at grazing incidence, so the
             * outermost pixel of the bevel is a mirror and the one after it
             * is not -- a hairline, all the way round, which is the cue that
             * makes this read as an object with thickness instead of a soft
             * patch. The lerp is toward the environment rather than toward
             * white, and it REPLACES what it reflects, so the slightly darker
             * pixel just inside the bright one is the same computation.
             * See glass.c for the table and for which part of this is faked. */
            /* The environment is directional only where the surface TILTS. The
             * flat middle of the panel faces the viewer, so it mirrors the same
             * thing everywhere, and it must not consult `facing` -- because in
             * the flat middle the normal is not measured, it is the guess two
             * branches up (`qx > qy ? horizontal : vertical`), and that guess
             * FLIPS across the 45-degree diagonal. Every earlier user of the
             * normal was multiplied by `band`, which is zero past 15 px, so the
             * discontinuity had never reached a pixel. R0 reflectance reaches
             * every pixel, and the first version of this line put a visible
             * diagonal seam down the middle of Finder's sidebar: 4% of an 87
             * level swing is 3 levels, and 3 levels on flat white is a line. */
            int tilt = depth < E ? tiltt[depth] : 0;
            int env = 176 + (facing - 128) * tilt / 256;
            if (env < 96)  env = 96;
            if (env > 250) env = 250;
            int fr = glass_fres[di];
            /* The wide wash stays, at less than half its old strength: it is
             * the body sheen, not the edge. Turning it up was the thing that
             * could not work -- a 15-pixel gradient is not a 1-pixel line. */
            int hi = facing * band / 256; hi = hi * hi / 256; hi = hi * SPEC / 256;
            int op = (nx * 154 + ny * 205) / 256; if (op < 0) op = 0;
            int sh = op * band / 256; sh = sh * sh / 256; sh = sh * 46 / 256;
            glass_shade(&r, &gg, &b, tr, tg, tb, ta, env, fr, hi, sh);
            glass_store(ctx, &s->px[(long)(y + j) * s->w + (x + i)], r, gg, b, gcov);
        }
    }
}

/* Blit a straight-RGBA source (sw x sh) into the dest rect (dx,dy,dw,dh) of the
 * current target with nearest-neighbour scaling and per-pixel alpha. The loops
 * are clipped to the visible target region (in 64-bit, so user-supplied dx/dw
 * can't overflow the math) WITHOUT rescaling: sx/sy are still computed against
 * the original dw/dh, so a partially off-screen blit crops exactly as before.
 * This is what bounds SYS_GUI_BLIT: an unclipped dw*dh double loop (up to
 * ~(2^31)^2 iterations) inside the syscall gate would freeze the machine. */
void ol_display_blit_rgba(struct ol_display *ctx, int dx, int dy, int dw, int dh, const uint8_t *rgba, int sw, int sh)
{
    struct ol_pixel_target *t = &ctx->target;
    if (!rgba || !t->px || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    long i0 = dx < 0 ? -(long)dx : 0, i1 = dw;
    long j0 = dy < 0 ? -(long)dy : 0, j1 = dh;
    if (i1 > (long)t->w - dx) i1 = (long)t->w - dx;
    if (j1 > (long)t->h - dy) j1 = (long)t->h - dy;
    if (t->clip_on) {                           /* clamp to the target's scissor too */
        if (t->clx0 - dx > i0) i0 = t->clx0 - dx;
        if (t->cly0 - dy > j0) j0 = t->cly0 - dy;
        if (t->clx1 - dx < i1) i1 = t->clx1 - dx;
        if (t->cly1 - dy < j1) j1 = t->cly1 - dy;
    }
    if (i0 >= i1 || j0 >= j1) return;
    for (long j = j0; j < j1; j++) {
        int sy = (int)(j * sh / dh);
        for (long i = i0; i < i1; i++) {
            int sx = (int)(i * sw / dw);
            const uint8_t *p = rgba + ((sy * sw + sx) * 4);
            int a = p[3];
            int px = (int)(dx + i), py = (int)(dy + j);
            if (!a) continue;
            if (a >= 255) { ol_display_put(ctx, px, py, ol_display_rgb(ctx, p[0], p[1], p[2])); continue; }
            int br, bg, bb; unpack(ctx, display_get(ctx, px, py), &br, &bg, &bb);
            int nr = (p[0] * a + br * (255 - a)) / 255;
            int ng = (p[1] * a + bg * (255 - a)) / 255;
            int nb = (p[2] * a + bb * (255 - a)) / 255;
            ol_display_put(ctx, px, py, ol_display_rgb(ctx, (uint8_t)nr, (uint8_t)ng, (uint8_t)nb));
        }
    }
}


int ol_display_bind(struct ol_display *ctx,const struct ol_pixel_target *t,unsigned r,unsigned g,unsigned b)
{
    if(!ctx||!t||!t->px||t->w<=0||t->h<=0||t->w>GFX_MAX_W||t->h>GFX_MAX_W||
       r>24||g>24||b>24||((255u<<r)&(255u<<g))||((255u<<r)&(255u<<b))||((255u<<g)&(255u<<b)))return OL_ARGUMENT;
    ctx->target=*t;ctx->rpos=r;ctx->gpos=g;ctx->bpos=b;ctx->error=0;ctx->batches++;return OL_OK;
}
void ol_display_destroy(struct ol_display *ctx)
{
    if(!ctx)return;
    display_free(ctx,blur_scratch);display_free(ctx,glass_buf);display_free(ctx,glass_line);
    blur_scratch=0;glass_buf=0;glass_line=0;
    blur_scratch_n=glass_buf_n=glass_line_n=0;
}

int ol_display_pack_argb(struct ol_display *ctx, uint32_t *out, const unsigned char *rgba, unsigned n)
{
    if (!ctx || !out || !rgba || ctx->rpos > 16 || ctx->gpos > 16 || ctx->bpos > 16)
        return OL_ARGUMENT;
    for (unsigned i=0;i<n;i++,rgba+=4)
        out[i]=rgba[3] ? ((uint32_t)rgba[3]<<24)|ol_display_rgb(ctx,rgba[0],rgba[1],rgba[2]) : 0;
    return OL_OK;
}
void ol_display_blit_argb(struct ol_display *ctx,int x,int y,const uint32_t *p,int w,int h,int stride)
{
    int x0,y0,x1,y1;
    if (!ctx || !p || stride < w || ctx->rpos > 16 || ctx->gpos > 16 || ctx->bpos > 16 ||
        !clip_ij(ctx,x,y,w,h,&x0,&y0,&x1,&y1)) return;
    for(int j=y0;j<y1;j++)for(int i=x0;i<x1;i++) {
        uint32_t s=p[(long)j*stride+i],*d=ctx->target.px+(long)(y+j)*ctx->target.w+x+i;
        int a=s>>24;
        if(!a)continue;
        if(a==255){*d=s&0xffffffu;continue;}
        int sr,sg,sb,dr,dg,db;unpack(ctx,s,&sr,&sg,&sb);unpack(ctx,*d,&dr,&dg,&db);
        *d=ol_display_rgb(ctx,(sr*a+dr*(255-a))/255,(sg*a+dg*(255-a))/255,(sb*a+db*(255-a))/255);
    }
}
/* Uniform layer alpha is sampled without rewriting every source alpha byte.
 * Clipping is a loop bound, so cost follows the damaged destination region. */
void ol_display_blit_surface_alpha(struct ol_display *ctx,int x,int y,int w,int h,
                                   const struct ol_pixel_target *src,int alpha)
{
    int x0,y0,x1,y1;
    if(!ctx||!src||!src->px||src->w<=0||src->h<=0||alpha<=0||alpha>255||
       !clip_ij(ctx,x,y,w,h,&x0,&y0,&x1,&y1))return;
    for(int j=y0;j<y1;j++) {
        const uint32_t *row=src->px+((long)j*src->h/h)*src->w;
        uint32_t *dst=ctx->target.px+(long)(y+j)*ctx->target.w+x;
        for(int i=x0;i<x1;i++) {
            uint32_t s=row[(long)i*src->w/w];int sr,sg,sb,dr,dg,db;
            unpack(ctx,s,&sr,&sg,&sb);unpack(ctx,dst[i],&dr,&dg,&db);
            dst[i]=ol_display_rgb(ctx,(sr*alpha+dr*(255-alpha))/255,
                (sg*alpha+dg*(255-alpha))/255,(sb*alpha+db*(255-alpha))/255);
        }
    }
}
