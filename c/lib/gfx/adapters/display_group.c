/* ol_display snapshot groups: opacity groups and rounded-rect clips on opaque
 * packed targets. The contract, the nesting and scratch rules and the kernel
 * clip caveat are in openlogit_display_group.h; this file is the mechanism.
 *
 * Snapshot layout (g->snap, uint32 pixels):
 *   opacity < 255  the whole rect, row-major, stride g->w, indexed by
 *                  rect-local (i, j). Rect pixels outside the clip that was in
 *                  force at begin are neither saved nor written back.
 *   opacity == 255 (OL_DISPLAY_GROUP_CORNERS) the four corner tiles in
 *                  TL, TR, BR, BL order, tile k being radius[k] x radius[k]
 *                  pixels, row-major, indexed by tile-local (i, j). Only the
 *                  corners can have weight < 1, so only they are saved.
 *
 * No file-scope state (see the rule above struct ol_display): everything lives
 * in the caller's struct and buffer or in display_scratch(OL_DS_GROUP). */
#include "openlogit_display_group.h"
#include "display_pixels.h"
#include "gfx_mask.h"
#include "gfx_math.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define DG_NEON 1
#endif

/* Weights are opacity (0..255) times coverage (0..255): 255*255 is "keep the
 * drawn pixel", 0 is "restore the snapshot". */
#define DG_ONE 65025u
/* Rect origins beyond this are refused, so rect-local + tile offsets
 * (<= OL_DISPLAY_MAX_SIZE) never overflow int. */
#define DG_COORD_MAX (1 << 30)
/* Analytic corner coverage for radii past GFX_MASK_MAX: DG_SUB sub-rows with
 * exact horizontal coverage, fixed point 1/DG_FP px. */
#define DG_SUB 8
#define DG_FP 4096

struct dg_rect { int x0, y0, x1, y1; };

static int dg_valid(int w, int h, int opacity)
{
    return w >= 0 && h >= 0 && w <= OL_DISPLAY_MAX_SIZE && h <= OL_DISPLAY_MAX_SIZE &&
           opacity >= 0 && opacity <= 255;
}

/* The clamp ol_display_round_rect applies (corner_mask_for): min(w,h)/2. */
static int dg_radii(int w, int h, const int *in, int out[4])
{
    int lim = (w < h ? w : h) / 2;
    for (int k = 0; k < 4; k++) {
        int r = in ? in[k] : 0;
        if (r < 0) return 0;
        out[k] = r > lim ? lim : r;
    }
    return 1;
}

static unsigned long dg_need(int w, int h, const int rad[4], int opacity)
{
    if (opacity < 255) return (unsigned long)w * (unsigned long)h * 4u;
    unsigned long n = 0;
    for (int k = 0; k < 4; k++) n += (unsigned long)rad[k] * (unsigned long)rad[k] * 4u;
    return n;
}

unsigned long ol_display_group_bytes(int w, int h, const int radius[4], int opacity)
{
    int rad[4];
    if (!dg_valid(w, h, opacity) || !dg_radii(w, h, radius, rad)) return 0;
    return dg_need(w, h, rad, opacity);
}

/* The pixels a group may touch: its rect, the target and the clip that was in
 * force at begin. Canonical when empty, so end() can compare it with the clip
 * begin() installed. */
static void dg_effective(const struct ol_pixel_target *t, const struct ol_display_group *g,
                         struct dg_rect *e)
{
    int x0 = g->x, y0 = g->y, x1 = g->x + g->w, y1 = g->y + g->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    if (g->prev_clip_on) {
        if (g->prev_clx0 > x0) x0 = g->prev_clx0;
        if (g->prev_cly0 > y0) y0 = g->prev_cly0;
        if (g->prev_clx1 < x1) x1 = g->prev_clx1;
        if (g->prev_cly1 < y1) y1 = g->prev_cly1;
    }
    if (x1 <= x0 || y1 <= y0) { x1 = x0; y1 = y0; }
    e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1;
}

/* Tile (tx,ty,tw,th) in rect-local pixels, intersected with e, as device
 * coordinates. Returns 0 when nothing survives. */
static int dg_box(const struct ol_display_group *g, const struct dg_rect *e,
                  int tx, int ty, int tw, int th, struct dg_rect *b)
{
    b->x0 = g->x + tx; b->y0 = g->y + ty; b->x1 = b->x0 + tw; b->y1 = b->y0 + th;
    if (b->x0 < e->x0) b->x0 = e->x0;
    if (b->y0 < e->y0) b->y0 = e->y0;
    if (b->x1 > e->x1) b->x1 = e->x1;
    if (b->y1 > e->y1) b->y1 = e->y1;
    return b->x0 < b->x1 && b->y0 < b->y1;
}

/* Save target pixels of tile (tx,ty,tw,th) into buf (row stride tw). */
static void dg_save(const struct ol_pixel_target *t, const struct ol_display_group *g,
                    const struct dg_rect *e, int tx, int ty, int tw, int th, uint32_t *buf)
{
    struct dg_rect b;
    if (!dg_box(g, e, tx, ty, tw, th, &b)) return;
    int ox = g->x + tx, oy = g->y + ty;
    for (int y = b.y0; y < b.y1; y++) {
        const uint32_t *src = t->px + (long)y * t->w + b.x0;
        uint32_t *dst = buf + (long)(y - oy) * tw + (b.x0 - ox);
        for (int i = 0; i < b.x1 - b.x0; i++) dst[i] = src[i];
    }
}

/* Corner tile origin, rect-local. k: 0 TL, 1 TR, 2 BR, 3 BL. */
static int dg_tx(const struct ol_display_group *g, int k) { return (k == 1 || k == 2) ? g->w - g->radius[k] : 0; }
static int dg_ty(const struct ol_display_group *g, int k) { return k >= 2 ? g->h - g->radius[k] : 0; }

int ol_display_group_begin(struct ol_display *ctx, struct ol_display_group *g,
                           int x, int y, int w, int h, const int radius[4],
                           int opacity, uint32_t *snap, unsigned long snap_bytes)
{
    if (g) g->flags = 0;
    if (!ctx || !g || !ctx->target.px) return OL_ARGUMENT;
    if (!dg_valid(w, h, opacity) || x < -DG_COORD_MAX || x > DG_COORD_MAX ||
        y < -DG_COORD_MAX || y > DG_COORD_MAX) return OL_ARGUMENT;
    int rad[4];
    if (!dg_radii(w, h, radius, rad)) return OL_ARGUMENT;

    unsigned long need = dg_need(w, h, rad, opacity);
    unsigned flags = OL_DISPLAY_GROUP_ACTIVE | (opacity == 255 ? OL_DISPLAY_GROUP_CORNERS : 0u);
    if (need) {
        if (!snap) {
            snap = display_scratch(ctx, OL_DS_GROUP, need);
            if (!snap) return OL_LIMIT;
            snap_bytes = need;
            flags |= OL_DISPLAY_GROUP_SCRATCH;
        } else if (snap_bytes < need) {
            return OL_LIMIT;
        }
    }

    struct ol_pixel_target *t = &ctx->target;
    g->x = x; g->y = y; g->w = w; g->h = h;
    for (int k = 0; k < 4; k++) g->radius[k] = rad[k];
    g->opacity = opacity;
    g->prev_clip_on = t->clip_on;
    g->prev_clx0 = t->clx0; g->prev_cly0 = t->cly0;
    g->prev_clx1 = t->clx1; g->prev_cly1 = t->cly1;
    g->snap = snap;
    g->snap_bytes = snap_bytes;

    struct dg_rect e;
    dg_effective(t, g, &e);
    if (need) {
        if (flags & OL_DISPLAY_GROUP_CORNERS) {
            unsigned long off = 0;
            for (int k = 0; k < 4; k++) {
                int r = rad[k];
                if (!r) continue;
                dg_save(t, g, &e, dg_tx(g, k), dg_ty(g, k), r, r, snap + off);
                off += (unsigned long)r * (unsigned long)r;
            }
        } else {
            dg_save(t, g, &e, 0, 0, w, h, snap);
        }
    }

    t->clip_on = 1;
    t->clx0 = e.x0; t->cly0 = e.y0; t->clx1 = e.x1; t->cly1 = e.y1;
    g->flags = flags;
    return OL_OK;
}

/* Exact, rounded snap + (drawn - snap) * W / 65025 per channel. The bits
 * outside the three channels (the X byte, which on a window surface can hold
 * wm2_backdrop's marker) are the DRAWN pixel's: they describe what is there
 * now, and a group never invents a marker. A pixel the children did not change
 * therefore comes back bit-exactly, and W == 0 restores the snapshot. */
static inline uint32_t dg_mix(const struct ol_display *ctx, uint32_t s, uint32_t d, unsigned W)
{
    if (W == 0 || s == d) return s;
    if (W >= DG_ONE) return d;
    unsigned iw = DG_ONE - W;
    unsigned r = ((s >> ctx->rpos) & 255u) * iw + ((d >> ctx->rpos) & 255u) * W;
    unsigned gg = ((s >> ctx->gpos) & 255u) * iw + ((d >> ctx->gpos) & 255u) * W;
    unsigned b = ((s >> ctx->bpos) & 255u) * iw + ((d >> ctx->bpos) & 255u) * W;
    r = (r + DG_ONE / 2) / DG_ONE;
    gg = (gg + DG_ONE / 2) / DG_ONE;
    b = (b + DG_ONE / 2) / DG_ONE;
    uint32_t chan = 255u << ctx->rpos | 255u << ctx->gpos | 255u << ctx->bpos;
    return r << ctx->rpos | gg << ctx->gpos | b << ctx->bpos | (d & ~chan);
}

/* A run of full-coverage pixels at constant opacity a (0 < a < 255): the
 * same result as dg_mix(s, d, a * 255) for every pixel. When the three
 * channels sit at bits 0, 8 and 16 in any order (BGRX, RGBX) every byte is
 * blended alike: round(x/255) is (t + (t >> 8)) >> 8 with t = x + 128, exact
 * for every x <= 255*255, and the fourth byte gets weight 255 on the drawn
 * pixel, which is dg_mix's rule with no select. Under clang that runs as
 * 16-byte vectors (SSE2 in the kernel, NEON on an arm64 host): this loop IS
 * the cost of an opacity group (test-display-snapshot-group-budget), and the
 * 32-bit SWAR form below it, the tail and the non-clang path, is about twice
 * as slow. */
#if defined(__clang__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define DG_VEC 1
typedef uint8_t dg_u8x16 __attribute__((vector_size(16)));
typedef uint8_t dg_u8x8 __attribute__((vector_size(8)));
typedef uint16_t dg_u16x8 __attribute__((vector_size(16)));

static inline dg_u16x8 dg_lo(dg_u8x16 v)
{ return __builtin_convertvector(__builtin_shufflevector(v, v, 0, 1, 2, 3, 4, 5, 6, 7), dg_u16x8); }
static inline dg_u16x8 dg_hi(dg_u8x16 v)
{ return __builtin_convertvector(__builtin_shufflevector(v, v, 8, 9, 10, 11, 12, 13, 14, 15), dg_u16x8); }
#endif

static void dg_span(const struct ol_display *ctx, uint32_t *d, const uint32_t *s, int n, unsigned a)
{
    unsigned ia = 255u - a;
    if (((1u << ctx->rpos) | (1u << ctx->gpos) | (1u << ctx->bpos)) != 0x10101u) {
        for (int i = 0; i < n; i++) d[i] = dg_mix(ctx, s[i], d[i], a * 255u);
        return;
    }
    int i = 0;
#ifdef DG_VEC
    /* Per-byte weights, X byte last in each pixel (little endian); built
     * from uint8_t so the compiler sees 8x8 -> 16 bit products (umull). */
    const uint8_t a8 = (uint8_t)a, i8 = (uint8_t)ia;
    const dg_u8x16 wa = { a8, a8, a8, 255, a8, a8, a8, 255, a8, a8, a8, 255, a8, a8, a8, 255 };
    const dg_u8x16 wi = { i8, i8, i8, 0, i8, i8, i8, 0, i8, i8, i8, 0, i8, i8, i8, 0 };
    const dg_u16x8 va = dg_lo(wa), via = dg_lo(wi), half = (dg_u16x8){ 0 } + (uint16_t)128;
#ifdef DG_NEON
    /* The same per-byte rule as NEON widening multiply-accumulates; the
     * rounded divide by 255 is vrsra + vrshrn, ((t + ((t+128) >> 8)) + 128)
     * >> 8, the identity above (t + 128 cannot overflow: t <= 65025). About
     * 1.5x faster than what clang makes of the generic vectors below. */
    const uint8x16_t na = vld1q_u8((const uint8_t *)&wa), ni = vld1q_u8((const uint8_t *)&wi);
    for (; i + 8 <= n; i += 8) {
        uint8x16_t s0 = vld1q_u8((const uint8_t *)(s + i)), d0 = vld1q_u8((const uint8_t *)(d + i));
        uint8x16_t s1 = vld1q_u8((const uint8_t *)(s + i + 4)), d1 = vld1q_u8((const uint8_t *)(d + i + 4));
        uint16x8_t l0 = vmlal_u8(vmull_u8(vget_low_u8(s0), vget_low_u8(ni)), vget_low_u8(d0), vget_low_u8(na));
        uint16x8_t h0 = vmlal_high_u8(vmull_high_u8(s0, ni), d0, na);
        uint16x8_t l1 = vmlal_u8(vmull_u8(vget_low_u8(s1), vget_low_u8(ni)), vget_low_u8(d1), vget_low_u8(na));
        uint16x8_t h1 = vmlal_high_u8(vmull_high_u8(s1, ni), d1, na);
        l0 = vrsraq_n_u16(l0, l0, 8);
        h0 = vrsraq_n_u16(h0, h0, 8);
        l1 = vrsraq_n_u16(l1, l1, 8);
        h1 = vrsraq_n_u16(h1, h1, 8);
        vst1q_u8((uint8_t *)(d + i), vrshrn_high_n_u16(vrshrn_n_u16(l0, 8), h0, 8));
        vst1q_u8((uint8_t *)(d + i + 4), vrshrn_high_n_u16(vrshrn_n_u16(l1, 8), h1, 8));
    }
#endif
    for (; i + 4 <= n; i += 4) {
        dg_u8x16 sv, dv;
        __builtin_memcpy(&sv, s + i, 16);
        __builtin_memcpy(&dv, d + i, 16);
        dg_u16x8 xl = dg_lo(sv) * via + dg_lo(dv) * va + half;
        dg_u16x8 xh = dg_hi(sv) * via + dg_hi(dv) * va + half;
        xl = (xl + (xl >> 8)) >> 8;
        xh = (xh + (xh >> 8)) >> 8;
        dg_u8x8 nl = __builtin_convertvector(xl, dg_u8x8), nh = __builtin_convertvector(xh, dg_u8x8);
        dg_u8x16 out = __builtin_shufflevector(nl, nh, 0, 1, 2, 3, 4, 5, 6, 7,
                                               8, 9, 10, 11, 12, 13, 14, 15);
        __builtin_memcpy(d + i, &out, 16);
    }
#endif
    for (; i < n; i++) {
        uint32_t sv = s[i], dv = d[i];
        uint32_t rb = (sv & 0xFF00FFu) * ia + (dv & 0xFF00FFu) * a + 0x800080u;
        rb = ((rb + ((rb >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
        uint32_t gg = ((sv >> 8) & 0xFFu) * ia + ((dv >> 8) & 0xFFu) * a + 0x80u;
        gg = (gg + (gg >> 8)) >> 8;
        d[i] = rb | gg << 8 | (dv & 0xFF000000u);
    }
}

/* Coverage of corner pixel (cx, cy) of an r x r quarter circle whose centre is
 * the tile's inner corner (r, r) -- gfx_corner_fill's geometry, so (0,0) is the
 * pixel at the rect's outer corner. DG_SUB sub-rows with exact horizontal
 * coverage are accurate where the arc is steep; the circle is symmetric about
 * the tile diagonal, so the shallow half (cx > cy) is computed mirrored.
 * Within 0.5/255 of the true pixel area (8 sub-rows, measured for r=73..180). */
static int dg_arc_cov(int r, int cx, int cy)
{
    if (cx > cy) { int t = cx; cx = cy; cy = t; }
    long long rr = (long long)r * r;
    long long fx = r - cx, fy = r - cy;          /* pixel corner farthest from the centre */
    if (fx * fx + fy * fy <= rr) return 255;
    long long nx = fx - 1, ny = fy - 1;          /* nearest */
    if (nx * nx + ny * ny >= rr) return 0;
    long long R = (long long)r * DG_FP, sum = 0;
    for (int s = 0; s < DG_SUB; s++) {
        long long dy = R - ((long long)cy * DG_FP + (2 * s + 1) * (DG_FP / (2 * DG_SUB)));
        long long xb = R - (long long)gfx_isqrt((unsigned long long)(R * R - dy * dy));
        long long f = (long long)(cx + 1) * DG_FP - xb;
        sum += f < 0 ? 0 : f > DG_FP ? DG_FP : f;
    }
    return (int)((sum * 255 + DG_SUB * DG_FP / 2) / (DG_SUB * DG_FP));
}

static void dg_blend(struct ol_display *ctx, const struct ol_display_group *g, const struct dg_rect *e)
{
    struct ol_pixel_target *t = &ctx->target;
    const int *rad = g->radius;
    int corners = (g->flags & OL_DISPLAY_GROUP_CORNERS) != 0;

    /* Everything outside the corner tiles has coverage 255: weight = opacity.
     * At opacity 255 (corner layout) those pixels keep what was drawn. */
    if (!corners) {
        for (int y = e->y0; y < e->y1; y++) {
            int lj = y - g->y;
            int rl = lj < rad[0] ? rad[0] : lj >= g->h - rad[3] ? rad[3] : 0;
            int rr = lj < rad[1] ? rad[1] : lj >= g->h - rad[2] ? rad[2] : 0;
            int x0 = g->x + rl, x1 = g->x + g->w - rr;
            if (x0 < e->x0) x0 = e->x0;
            if (x1 > e->x1) x1 = e->x1;
            if (x0 >= x1) continue;
            uint32_t *drow = t->px + (long)y * t->w + x0;
            const uint32_t *srow = g->snap + (long)lj * g->w + (x0 - g->x);
            if (g->opacity == 0)
                for (int i = 0; i < x1 - x0; i++) drow[i] = srow[i];
            else
                dg_span(ctx, drow, srow, x1 - x0, (unsigned)g->opacity);
        }
    }

    unsigned long off = 0;
    for (int k = 0; k < 4; k++) {
        int r = rad[k];
        if (!r) continue;
        int tx = dg_tx(g, k), ty = dg_ty(g, k);
        int flipx = k == 1 || k == 2, flipy = k >= 2;
        struct dg_rect b;
        if (dg_box(g, e, tx, ty, r, r, &b)) {
            /* One corner at a time: the mask is a pointer into gfx_mask's
             * cache, and the next corner's lookup may evict it. */
            const unsigned char *m = r <= GFX_MASK_MAX ? gfx_mask_corner(GFX_MASK_FILL, r, r, 0) : 0;
            int ox = g->x + tx, oy = g->y + ty;
            for (int y = b.y0; y < b.y1; y++) {
                int tj = y - oy, cy = flipy ? r - 1 - tj : tj;
                uint32_t *drow = t->px + (long)y * t->w + b.x0;
                const uint32_t *srow = corners ? g->snap + off + (long)tj * r + (b.x0 - ox)
                                               : g->snap + (long)(y - g->y) * g->w + (b.x0 - g->x);
                for (int i = 0; i < b.x1 - b.x0; i++) {
                    int ti = b.x0 + i - ox, cx = flipx ? r - 1 - ti : ti;
                    int cov = m ? m[(long)cy * r + cx] : dg_arc_cov(r, cx, cy);
                    drow[i] = dg_mix(ctx, srow[i], drow[i], (unsigned)g->opacity * (unsigned)cov);
                }
            }
        }
        if (corners) off += (unsigned long)r * (unsigned long)r;
    }
}

int ol_display_group_end(struct ol_display *ctx, struct ol_display_group *g)
{
    if (!ctx || !g || !(g->flags & OL_DISPLAY_GROUP_ACTIVE)) return OL_ARGUMENT;
    struct ol_pixel_target *t = &ctx->target;
    int status = OL_OK, blend = t->px != 0 && g->snap != 0;

    struct dg_rect e;
    dg_effective(t, g, &e);
    /* LIFO: the clip must still be the one begin() installed. */
    if (!t->clip_on || t->clx0 != e.x0 || t->cly0 != e.y0 || t->clx1 != e.x1 || t->cly1 != e.y1)
        status = OL_ARGUMENT;
    /* A later scratch group grew (and so freed) this group's snapshot. */
    if ((g->flags & OL_DISPLAY_GROUP_SCRATCH) &&
        (ctx->scratch[OL_DS_GROUP] != (void *)g->snap || ctx->scratch_n[OL_DS_GROUP] < g->snap_bytes)) {
        status = OL_ARGUMENT;
        blend = 0;
    }
#ifndef OL_DISPLAY_GROUP_NO_RESTORE
    if (blend && e.x0 < e.x1) dg_blend(ctx, g, &e);
#else
    (void)blend; (void)dg_blend;   /* negative control: keep the drawn pixels */
#endif
    t->clip_on = g->prev_clip_on;
    t->clx0 = g->prev_clx0; t->cly0 = g->prev_cly0;
    t->clx1 = g->prev_clx1; t->cly1 = g->prev_cly1;
    g->flags &= ~OL_DISPLAY_GROUP_ACTIVE;
    return status;
}
