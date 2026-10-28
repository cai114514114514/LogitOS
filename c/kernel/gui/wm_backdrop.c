/* Backdrop regions (GUI ABI v2: SYS_GUI_BACKDROP, SYS_GUI_BACKDROP_CLEAR):
 * glass that shows the DESKTOP BEHIND a window -- a sidebar -- with the app's
 * own ink on top of it.
 *
 * THE MECHANISM (decided in wm_draw2.h, "BACKDROP FOREGROUND SEPARATION") is a
 * marker-byte ink mask. wm2_backdrop records the region and stores fill_rgb
 * with high byte LOGIT_BACKDROP_MARK into every surface pixel of its rounded
 * rect, directly (clipped to the surface, not to the app's clip). Any later
 * app draw rewrites the pixels it touches with high byte 0. The compositor
 * (wm.c, render_layers, for a window drawn at its own frame) then does, BEFORE
 * and INSTEAD OF the plain content blit:
 *   1. wm_backdrop_composite: each region's material (ol_display_liquid_glass_ex)
 *      over what the back buffer already holds there -- the wallpaper and
 *      every window beneath, i.e. the desktop behind this window;
 *   2. wm_backdrop_blit_content: the canvas copied 1:1, except that inside a
 *      region a pixel whose high byte is MARK is skipped, so the glass shows
 *      through it and the app's ink everywhere else.
 * Both run inside the window's rounded-corner group (wm.c, content_group_*), so
 * the glass is clipped to the window shape exactly as the content is.
 *
 * THE AUDIT: every path that stores an app-controlled value into a window
 * surface, and why none but wm2_backdrop can leave MARK in a high byte.
 *   - SYS_GUI_CLEAR and SYS_GUI_RECT stored the raw app value
 *     (ol_display_clear / ol_display_fill_rect store `color` unchanged), so
 *     an app colour 0x01xxxxxx forged MARK. wm.c now masks the colour with
 *     wm_app_rgb() (wm_cover.h, & 0xFFFFFF) in both cases.
 *   - wm_draw2.h's audit called RRECT, TEXT, TEXT_MONO, TEXT_RUN and ICON
 *     "display_rgb-packed". They are only where they BLEND: a fully covered
 *     pixel is stored raw (ol_display_round_rect's rr_fill, blit_glyph's
 *     a >= 255 store, blend_cov's ol_display_put), which forges MARK exactly
 *     like CLEAR. wm.c masks the app colour in those five cases too.
 *   - SYS_GUI_GLASS takes four tint bytes and stores display_rgb-packed
 *     pixels (glass_render / glass_render_solid): high byte 0.
 *   - SYS_GUI_BLIT and SYS_GUI_ICON(LOGIT_ICON_APP) unpack RGBA through
 *     display_rgb: high byte 0.
 *   - The v2 handlers mask every raw colour they take: wm_draw2_path.c
 *     (paint argb and stops, "& 0xFFFFFFu"), wm_draw2_text.c (run color),
 *     wm_draw2_state.c (BLIT2 unpacks RGBA). GROUP_END blends through
 *     display_rgb and copies untouched snapshot pixels back as they were: it
 *     can RESTORE a MARK this file wrote, never create one.
 *   - ol_display_blit_surface copies whole pixels, high byte included. Every
 *     compositor blit (fb_blit_surface[_scaled], anim_blit_fade, the scaled
 *     thumbnail blit, wm_backdrop_blit_content) READS a window surface into
 *     the back buffer or a scratch surface and never writes a window surface;
 *     no path copies one app's surface into another's. The one surface-to-
 *     surface copy into a window is win_apply_size's ol_bitmap_reshape, which
 *     carries the window's OWN old pixels (MARK included) into its resized
 *     canvas -- and the resize drops every region (wm2_win_reset), so those
 *     marks are shown as their fill_rgb until the app redraws.
 * Only wm2_backdrop writes MARK.
 *
 * STATE AND LOCKING. The region table is kernel state indexed by window slot
 * (WM2_MAX_WINDOWS x LOGIT_BACKDROP_MAX), not c/lib/gfx state, so it is not per
 * drawing context; graphics_lock (fb_graphics_lock, a sleeping mutex)
 * serialises it:
 *   - written by SYS_GUI_BACKDROP (graphics_lock; wm_lock is dropped across a
 *     pixel call), SYS_GUI_BACKDROP_CLEAR (graphics_lock + wm_lock), and
 *     wm2_backdrop_win_reset from SYS_GUI_CREATE, win_apply_size and reap
 *     (graphics_lock + wm_lock each);
 *   - read by the compositor inside wm_render (graphics_lock + wm_lock held for
 *     the whole frame; the band workers of par_render read it while the
 *     compositor holds both, and never write it);
 *   - ALSO read, for damage only, by dirty_win under wm_lock alone (input
 *     pass). A racing SYS_GUI_BACKDROP can tear that read; the cost is one
 *     mis-sized damage box, because dmg_expand re-reads the table under
 *     graphics_lock before anything is drawn and grows every rectangle that
 *     touches a region to the region plus its halo.
 * Composite scratch is the caller's display (the band's own fb context
 * display: glass_buf/glass_line); this file keeps no pixel scratch.
 *
 * GEOMETRY. x, y, w, h and radius[] are 24.8 window-content points, converted
 * like a v2 clip (wm2_rect24, LOGIT_RECT_SNAP honoured, each edge rounded to
 * the nearest device pixel) and clipped to the surface at record time; an
 * empty result removes the id. The material takes ONE radius: the largest of
 * radius[] (clamped to half the short side), with a side CUT square
 * (OL_GLASS_CUT_*) when both of its corners are 0 -- a sidebar asking for
 * {0, r, r, 0} gets a square left edge and rounded right corners. The mark
 * fill uses that same shape (pixel centre inside the arc).
 *
 * LIMITATIONS.
 *   - AA fringes of app ink inside a region are blended against fill_rgb, not
 *     the live material (logit_abi.h).
 *   - A window mid-resize (canvas a size behind its frame) is stretched by
 *     the plain blit and shows fill_rgb for those frames; an opaque scaled
 *     thumbnail (Expose, dock fly) the same.
 *   - A FADING window composites its regions (scaled) inside its corner group,
 *     but anim_blit_fade blends the whole canvas over them: it skips the
 *     marker test, so the region reads as glass seen through the fading
 *     fill_rgb.
 *   - Clearing a region does not repaint it: its MARK pixels show fill_rgb
 *     (copied raw outside any live region) until the app draws over them.
 *   - Overlapping regions composite in id order; a later one frosts the
 *     earlier one's glass.
 *
 * NEGATIVE CONTROL: -DWM_BACKDROP_SELF_ONLY frosts the app's own pixels: the
 * region's fill colour is laid over its sample extent inside the window
 * before the glass, so the material samples the canvas instead of the
 * desktop. make test-wm-backdrop-regions-negctl (host, 'FAIL desktop-sample')
 * and test-wm-backdrop-regions-guest-negctl (BACKDROPCTL=1, the wallpaper
 * variance check) must fail with it. */
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "wm_cover.h"
#include "wm_bands.h"
#include "openlogit_glass_material.h"

/* Adaptive-tint flag bits of logit_glass2.flags / logit_backdrop2.flags (design
 * P16). ui-wm-abi defines them in logit_abi.h; these fallbacks carry the same
 * values so this file builds either side of that merge. They map onto
 * ol_glass_material.flags OL_GLASS_ADAPT_SMOOTH (1) / OL_GLASS_ADAPT_FG (2). */
#ifndef LOGIT_GLASS2_F_ADAPT_SMOOTH
#define LOGIT_GLASS2_F_ADAPT_SMOOTH (1u << 1)
#endif
#ifndef LOGIT_GLASS2_F_ADAPT_FG
#define LOGIT_GLASS2_F_ADAPT_FG (1u << 2)
#endif
_Static_assert(LOGIT_GLASS2_F_ADAPT_SMOOTH == (1u << 1), "ADAPT_SMOOTH is bit 1");
_Static_assert(LOGIT_GLASS2_F_ADAPT_FG == (1u << 2), "ADAPT_FG is bit 2");
_Static_assert(OL_GLASS_ADAPT_SMOOTH == 1 && OL_GLASS_ADAPT_FG == 2, "material flag values");
#define GLASS2_ADAPT_MASK (LOGIT_GLASS2_F_ADAPT_SMOOTH | LOGIT_GLASS2_F_ADAPT_FG)

/* The ring-3 flag bits as ol_glass_material.flags. */
static inline __attribute__((unused)) uint8_t bd_adapt_flags(unsigned f)
{
    return (uint8_t)(((f & LOGIT_GLASS2_F_ADAPT_SMOOTH) ? OL_GLASS_ADAPT_SMOOTH : 0) |
                     ((f & LOGIT_GLASS2_F_ADAPT_FG) ? OL_GLASS_ADAPT_FG : 0));
}

/* logit_backdrop2 has no fg_rgb field, so ADAPT_FG takes the ink colour from
 * the region's own fill_rgb: a light fill (the app is in its light theme) is
 * read with black ink, a dark fill with white ink. The panel is then held to
 * WCAG >= 4.5:1 against that ink over whatever the wallpaper is. */
static __attribute__((unused)) uint32_t bd_adapt_fg(uint32_t fill)
{
    unsigned y = (54u * ((fill >> 16) & 255) + 183u * ((fill >> 8) & 255) +
                  19u * (fill & 255)) >> 8;
    return y >= 128 ? 0x000000u : 0xFFFFFFu;
}

const unsigned wm2_backdrop_info = LOGIT_GUI2_F_BACKDROP;

struct wm_bd_reg {
    int used;
    struct drect r;               /* window-surface DEVICE px, inside the surface */
    int radius[4];                /* device px, TL TR BR BL, as the app asked */
    int rad;                      /* the material's one radius, clamped */
    unsigned cut;                 /* OL_GLASS_CUT_* sides whose corners are square */
    unsigned flags;               /* the app's flags (LOGIT_RECT_SNAP | adapt bits) */
    uint32_t fill;                /* fill_rgb packed for the surface, high byte 0 */
    struct ol_glass_material mat;
};
static struct wm_bd_reg g_bd[WM2_MAX_WINDOWS][LOGIT_BACKDROP_MAX];
static int g_bd_n[WM2_MAX_WINDOWS];   /* live regions per slot: the fast "none" test */
static int g_bd_sw[WM2_MAX_WINDOWS], g_bd_sh[WM2_MAX_WINDOWS];   /* the canvas they were clipped to */

static int bd_slot_ok(int slot) { return slot >= 0 && slot < WM2_MAX_WINDOWS; }
static int bd_round_px(long long v24) { return wm2_sat(wm2_floordiv(v24 + 128, 256)); }
static int bd_in(int v, int lo, int hi) { return v >= lo && v <= hi; }
static int bd_hit(const struct drect *a, const struct drect *b)
{
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}
static void bd_and(struct drect *a, const struct drect *b)
{
    if (b->x0 > a->x0) a->x0 = b->x0;
    if (b->y0 > a->y0) a->y0 = b->y0;
    if (b->x1 < a->x1) a->x1 = b->x1;
    if (b->y1 < a->y1) a->y1 = b->y1;
}
static int bd_empty(const struct drect *a) { return a->x0 >= a->x1 || a->y0 >= a->y1; }

static void bd_drop(int slot, int id)
{
    if (g_bd[slot][id].used) { g_bd[slot][id].used = 0; g_bd_n[slot]--; }
}

/* The one radius and the square sides of a region (see GEOMETRY above). */
static void bd_shape(struct wm_bd_reg *g)
{
    int m = 0;
    for (int k = 0; k < 4; k++) if (g->radius[k] > m) m = g->radius[k];
    int w = g->r.x1 - g->r.x0, h = g->r.y1 - g->r.y0, s = w < h ? w : h;
    if (m > s / 2) m = s / 2;
    g->rad = m;
    g->cut = 0;
    if (m > 0) {
        if (!g->radius[0] && !g->radius[1]) g->cut |= OL_GLASS_CUT_TOP;
        if (!g->radius[1] && !g->radius[2]) g->cut |= OL_GLASS_CUT_RIGHT;
        if (!g->radius[2] && !g->radius[3]) g->cut |= OL_GLASS_CUT_BOTTOM;
        if (!g->radius[3] && !g->radius[0]) g->cut |= OL_GLASS_CUT_LEFT;
    }
}

/* Is local pixel (i, j) of a w x h shape with radius rad and square sides
 * `cut` inside it? Pixel centre against the corner arc. */
static int bd_inside(int i, int j, int w, int h, int rad, unsigned cut)
{
    if (rad <= 0) return 1;
    int cx, cy;
    if (i < rad) { if (cut & OL_GLASS_CUT_LEFT) return 1; cx = rad - i; }
    else if (i >= w - rad) { if (cut & OL_GLASS_CUT_RIGHT) return 1; cx = i - (w - rad) + 1; }
    else return 1;
    if (j < rad) { if (cut & OL_GLASS_CUT_TOP) return 1; cy = rad - j; }
    else if (j >= h - rad) { if (cut & OL_GLASS_CUT_BOTTOM) return 1; cy = j - (h - rad) + 1; }
    else return 1;
    /* centre offset from the arc centre is (cx - 1/2, cy - 1/2), doubled */
    long long dx = 2LL * cx - 1, dy = 2LL * cy - 1;
    return dx * dx + dy * dy <= 4LL * rad * rad;
}

/* Store fill | MARK << 24 into the region's surface pixels: the ink mask. */
static void bd_mark(struct ol_pixel_target *t, const struct wm_bd_reg *g)
{
    uint32_t v = g->fill | ((uint32_t)LOGIT_BACKDROP_MARK << 24);
    int w = g->r.x1 - g->r.x0, h = g->r.y1 - g->r.y0;
    for (int j = 0; j < h; j++) {
        uint32_t *row = t->px + (long)(g->r.y0 + j) * t->w + g->r.x0;
        int corner = g->rad > 0 && (j < g->rad || j >= h - g->rad);
        for (int i = 0; i < w; i++)
            if (!corner || bd_inside(i, j, w, h, g->rad, g->cut)) row[i] = v;
    }
}

long wm2_backdrop(struct wm2_env *e, long a, long b, long c)
{
    struct logit_backdrop2 bd;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &bd, a, sizeof bd);
    if (rc != OL_OK) return rc;
    if (!bd_slot_ok(e->slot)) return OL_STATE;
    /* ALPHA_LAYER shares bit 1 with LOGIT_GLASS2_F_ADAPT_SMOOTH in the ABI header, so
     * SMOOTH is refused here until ui-wm-abi renumbers ALPHA_LAYER; ADAPT_FG (bit 2)
     * is honoured. */
    if (bd.flags & LOGIT_BACKDROP_ALPHA_LAYER) return OL_UNSUPPORTED;   /* reserved */
    if (bd.flags & ~(LOGIT_RECT_SNAP | GLASS2_ADAPT_MASK)) return OL_ARGUMENT;
    for (int k = 0; k < 4; k++) if (bd.reserved[k]) return OL_ARGUMENT;
    if (bd.id < 0 || bd.id >= LOGIT_BACKDROP_MAX) return OL_ARGUMENT;
    if (bd.w < 0 || bd.h < 0) return OL_ARGUMENT;
    for (int k = 0; k < 4; k++) if (bd.radius[k] < 0) return OL_ARGUMENT;
    if (!bd_in(bd.opacity, 0, 255) || !bd_in(bd.tone, 0, 255) ||
        !bd_in(bd.blur_level, OL_GLASS_BLUR_REGULAR, OL_GLASS_BLUR_ULTRA) ||
        !bd_in(bd.rim, OL_GLASS_RIM_DIRECTIONAL, OL_GLASS_RIM_SYMMETRIC))
        return OL_ARGUMENT;
    if (!e->d || !e->d->target.px) return OL_STATE;
    struct ol_pixel_target *t = &e->d->target;
    int sw = e->surf_w < t->w ? e->surf_w : t->w;
    int sh = e->surf_h < t->h ? e->surf_h : t->h;

    struct wm_bd_reg g = {0};
    struct wm2_rect24 r24 = wm2_rect24(bd.x, bd.y, bd.w, bd.h, e->scale_pct,
                                       (bd.flags & LOGIT_RECT_SNAP) != 0);
    g.r.x0 = bd_round_px(r24.x);
    g.r.y0 = bd_round_px(r24.y);
    g.r.x1 = bd_round_px((long long)r24.x + r24.w);
    g.r.y1 = bd_round_px((long long)r24.y + r24.h);
    struct drect surf = { 0, 0, sw, sh };
    bd_and(&g.r, &surf);
    if (bd_empty(&g.r)) { bd_drop(e->slot, bd.id); return OL_OK; }
    for (int k = 0; k < 4; k++)
        g.radius[k] = bd_round_px(wm2_pt24(bd.radius[k], e->scale_pct));
    bd_shape(&g);
    g.flags = bd.flags;
    g.fill = ol_display_rgb(e->d, (uint8_t)(bd.fill_rgb >> 16), (uint8_t)(bd.fill_rgb >> 8),
                            (uint8_t)bd.fill_rgb) & 0xFFFFFFu;
    g.mat.tint = bd.tint_argb & 0xFFFFFFu;
    g.mat.opacity = (uint8_t)bd.opacity;
    g.mat.tone = (uint8_t)bd.tone;
    g.mat.blur = (uint8_t)bd.blur_level;
    g.mat.rim = (uint8_t)bd.rim;
#ifndef UI_ADAPT_NEGCTL_BD         /* control: drop the copy */
    g.mat.flags = bd_adapt_flags(bd.flags);
    g.mat.fg_rgb = bd_adapt_fg(bd.fill_rgb & 0xFFFFFFu);
#endif
    g.used = 1;
    if (!g_bd[e->slot][bd.id].used) g_bd_n[e->slot]++;
    g_bd_sw[e->slot] = sw;
    g_bd_sh[e->slot] = sh;
    g_bd[e->slot][bd.id] = g;
    bd_mark(t, &g_bd[e->slot][bd.id]);
    return OL_OK;
}

long wm2_backdrop_clear(struct wm2_env *e, long a, long b, long c)
{
    (void)b; (void)c;
    if (a < -1 || a >= LOGIT_BACKDROP_MAX) return OL_ARGUMENT;
    if (!bd_slot_ok(e->slot)) return OL_STATE;
    if (a >= 0) bd_drop(e->slot, (int)a);
    else for (int id = 0; id < LOGIT_BACKDROP_MAX; id++) bd_drop(e->slot, id);
    return OL_OK;
}

void wm2_backdrop_win_reset(int slot)
{
    if (!bd_slot_ok(slot)) return;
    for (int id = 0; id < LOGIT_BACKDROP_MAX; id++) g_bd[slot][id].used = 0;
    g_bd_n[slot] = 0;
}

int wm_backdrop_any(int slot)
{
    return bd_slot_ok(slot) && g_bd_n[slot] > 0;
}

int wm_backdrop_regions(int slot, struct wm2_region *out, int max)
{
    int n = 0;
    if (!bd_slot_ok(slot) || !out) return 0;
    for (int id = 0; id < LOGIT_BACKDROP_MAX && n < max; id++) {
        const struct wm_bd_reg *g = &g_bd[slot][id];
        if (!g->used) continue;
        out[n].id = id;
        out[n].r = g->r;
        for (int k = 0; k < 4; k++) out[n].radius[k] = g->radius[k];
        out[n].flags = g->flags;
        n++;
    }
    return n;
}

/* ---- placement: window surface -> back buffer ----------------------------- */

static int bd_place_ok(const struct wm_bd_place *pl)
{
    return pl && pl->sw > 0 && pl->sh > 0 && pl->dw > 0 && pl->dh > 0;
}
/* v * d / s for v >= 0: exact (v) when the placement is 1:1. */
static int bd_map(int v, int d, int s) { return (int)((long long)v * d / s); }

/* A region's device rect in the back buffer, and its radius there. */
static void bd_dev(const struct wm_bd_reg *g, const struct wm_bd_place *pl,
                   struct drect *o, int *rad)
{
    o->x0 = pl->ox + bd_map(g->r.x0, pl->dw, pl->sw);
    o->y0 = pl->oy + bd_map(g->r.y0, pl->dh, pl->sh);
    o->x1 = pl->ox + bd_map(g->r.x1, pl->dw, pl->sw);
    o->y1 = pl->oy + bd_map(g->r.y1, pl->dh, pl->sh);
    int r = bd_map(g->rad, pl->dw, pl->sw);
    int w = o->x1 - o->x0, h = o->y1 - o->y0, s = w < h ? w : h;
    if (r > s / 2) r = s / 2;
    if (r < 0) r = 0;
    *rad = r;
}

/* How far beyond its rect the material reads the backdrop. 0 under Reduce
 * Transparency: the solid path samples nothing. */
static int bd_halo(const struct wm_bd_reg *g, const struct drect *o, int rad, int reduce)
{
    if (reduce) return 0;
    return ol_display_glass_sample_halo_ex(o->x1 - o->x0, o->y1 - o->y0, rad, &g->mat);
}

int wm_backdrop_screen_rects(int slot, const struct wm_bd_place *pl, struct drect *out, int max)
{
    int n = 0;
    if (!bd_slot_ok(slot) || !bd_place_ok(pl) || !g_bd_n[slot]) return 0;
    for (int id = 0; id < LOGIT_BACKDROP_MAX && n < max; id++) {
        int rad;
        if (!g_bd[slot][id].used) continue;
        bd_dev(&g_bd[slot][id], pl, &out[n], &rad);
        if (!bd_empty(&out[n])) n++;
    }
    return n;
}

int wm_backdrop_cover_ok(const struct drect *R, int slot, const struct wm_bd_place *pl)
{
    struct drect reg[LOGIT_BACKDROP_MAX];
    int n = wm_backdrop_screen_rects(slot, pl, reg, LOGIT_BACKDROP_MAX);
    return wm_cover_ok_regions(R, reg, n);
}

int wm_backdrop_extent(int slot, const struct wm_bd_place *pl, int reduce, struct drect *out)
{
    int any = 0;
    if (!bd_slot_ok(slot) || !bd_place_ok(pl) || !g_bd_n[slot]) return 0;
    for (int id = 0; id < LOGIT_BACKDROP_MAX; id++) {
        const struct wm_bd_reg *g = &g_bd[slot][id];
        struct drect o;
        int rad;
        if (!g->used) continue;
        bd_dev(g, pl, &o, &rad);
        if (bd_empty(&o)) continue;
        int halo = bd_halo(g, &o, rad, reduce);
        o.x0 -= halo; o.y0 -= halo; o.x1 += halo; o.y1 += halo;
        if (!any) { *out = o; any = 1; continue; }
        if (o.x0 < out->x0) out->x0 = o.x0;
        if (o.y0 < out->y0) out->y0 = o.y0;
        if (o.x1 > out->x1) out->x1 = o.x1;
        if (o.y1 > out->y1) out->y1 = o.y1;
    }
    return any;
}

int wm_backdrop_dmg_grow(struct drect *r, int slot, const struct wm_bd_place *pl,
                         int reduce, int scr_w, int scr_h)
{
    int grew = 0;
    if (!bd_slot_ok(slot) || !bd_place_ok(pl) || !g_bd_n[slot]) return 0;
    for (int id = 0; id < LOGIT_BACKDROP_MAX; id++) {
        const struct wm_bd_reg *g = &g_bd[slot][id];
        struct drect p;
        int rad;
        if (!g->used) continue;
        bd_dev(g, pl, &p, &rad);
        if (bd_empty(&p)) continue;
        int halo = bd_halo(g, &p, rad, reduce);
        p.x0 -= halo; p.y0 -= halo; p.x1 += halo; p.y1 += halo;
        /* ON SCREEN, like wm.c's rect_or_onscreen: clamping the box (not the
         * result) keeps the expansion convergent at the screen edge. */
        if (p.x0 < 0) p.x0 = 0;
        if (p.y0 < 0) p.y0 = 0;
        if (p.x1 > scr_w) p.x1 = scr_w;
        if (p.y1 > scr_h) p.y1 = scr_h;
        if (bd_empty(&p) || !bd_hit(r, &p)) continue;
        if (p.x0 >= r->x0 && p.y0 >= r->y0 && p.x1 <= r->x1 && p.y1 <= r->y1) continue;
        if (p.x0 < r->x0) r->x0 = p.x0;
        if (p.y0 < r->y0) r->y0 = p.y0;
        if (p.x1 > r->x1) r->x1 = p.x1;
        if (p.y1 > r->y1) r->y1 = p.y1;
        grew = 1;
    }
    return grew;
}

int wm_backdrop_nocut(int slot, const struct wm_bd_place *pl, int reduce,
                      const struct drect *R, struct wm_yrange *nc, int n, int max)
{
    if (!bd_slot_ok(slot) || !bd_place_ok(pl) || !g_bd_n[slot]) return n;
    for (int id = 0; id < LOGIT_BACKDROP_MAX; id++) {
        const struct wm_bd_reg *g = &g_bd[slot][id];
        struct drect o;
        int rad;
        if (!g->used) continue;
        bd_dev(g, pl, &o, &rad);
        if (bd_empty(&o)) continue;
        n = wm_bands_nocut_add(nc, n, max, o.x0, o.y0, o.x1, o.y1, bd_halo(g, &o, rad, reduce),
                               R->x0, R->y0, R->x1, R->y1);
    }
    return n;
}

/* ---- the composite --------------------------------------------------------- */

void wm_backdrop_composite_at(const struct drect *R, int win_slot,
                              const struct wm_bd_place *pl, struct ol_display *d)
{
    if (!R || !d || !d->target.px || !bd_slot_ok(win_slot) || !bd_place_ok(pl) ||
        !g_bd_n[win_slot])
        return;
    struct ol_pixel_target *t = &d->target;
    struct drect c = *R;
    struct drect content = { pl->ox, pl->oy, pl->ox + pl->dw, pl->oy + pl->dh };
    struct drect screen = { 0, 0, t->w, t->h };
    bd_and(&c, &content);
    bd_and(&c, &screen);
    if (t->clip_on) {
        struct drect clip = { t->clx0, t->cly0, t->clx1, t->cly1 };
        bd_and(&c, &clip);
    }
    if (bd_empty(&c)) return;
    /* Writes are clipped to R, the window's content and the caller's clip; the
     * glass still SAMPLES its whole halo, which dmg_expand and the band
     * splitter guarantee was composited in this same pass. */
    int on = t->clip_on, x0 = t->clx0, y0 = t->cly0, x1 = t->clx1, y1 = t->cly1;
    t->clip_on = 1; t->clx0 = c.x0; t->cly0 = c.y0; t->clx1 = c.x1; t->cly1 = c.y1;
    for (int id = 0; id < LOGIT_BACKDROP_MAX; id++) {
        const struct wm_bd_reg *g = &g_bd[win_slot][id];
        struct drect o;
        int rad;
        if (!g->used) continue;
        bd_dev(g, pl, &o, &rad);
        if (bd_empty(&o) || !bd_hit(&o, &c)) continue;
#ifdef WM_BACKDROP_SELF_ONLY
        /* THE CONTROL: frost the app's own pixels -- lay the canvas's fill
         * under the region's whole sample extent inside the window first. */
        if (!d->reduce_transparency) {    /* the solid path samples nothing */
            int halo = bd_halo(g, &o, rad, 0);
            ol_display_fill_rect(d, o.x0 - halo, o.y0 - halo, o.x1 - o.x0 + 2 * halo,
                                 o.y1 - o.y0 + 2 * halo, g->fill);
        }
#endif
        ol_display_liquid_glass_ex(d, o.x0, o.y0, o.x1 - o.x0, o.y1 - o.y0, rad, g->cut, &g->mat);
    }
    t->clip_on = on; t->clx0 = x0; t->cly0 = y0; t->clx1 = x1; t->cly1 = y1;
}

void wm_backdrop_composite(const struct drect *R, int win_slot, int dx, int dy,
                           struct ol_display *d)
{
    /* 1:1, over the canvas the regions were recorded on (a resize drops
     * them all, so it is the current one). */
    if (!bd_slot_ok(win_slot)) return;
    struct wm_bd_place pl = { dx, dy, g_bd_sw[win_slot], g_bd_sh[win_slot],
                              g_bd_sw[win_slot], g_bd_sh[win_slot] };
    wm_backdrop_composite_at(R, win_slot, &pl, d);
}

/* Copy n pixels raw (the plain blit), or skipping MARK pixels: a branch-free
 * select per pixel, so the loop vectorises. */
static void bd_copy(uint32_t *d, const uint32_t *s, int n)
{
    for (int x = 0; x < n; x++) d[x] = s[x];
}
static void bd_copy_ink(uint32_t *d, const uint32_t *s, int n)
{
    for (int x = 0; x < n; x++) {
        uint32_t p = s[x];
        d[x] = (p >> 24) == LOGIT_BACKDROP_MARK ? d[x] : p;
    }
}

void wm_backdrop_blit_content(const struct drect *R, int win_slot, int dx, int dy,
                              struct ol_display *d, const struct ol_pixel_target *src)
{
    if (!R || !d || !d->target.px || !src || !src->px || src->w <= 0 || src->h <= 0) return;
    struct ol_pixel_target *t = &d->target;
    struct drect c = *R;
    struct drect content = { dx, dy, dx + src->w, dy + src->h };
    struct drect screen = { 0, 0, t->w, t->h };
    bd_and(&c, &content);
    bd_and(&c, &screen);
    if (t->clip_on) {
        struct drect clip = { t->clx0, t->cly0, t->clx1, t->cly1 };
        bd_and(&c, &clip);
    }
    if (bd_empty(&c)) return;
    /* The live regions in back-buffer coordinates, clipped to c. */
    struct drect reg[LOGIT_BACKDROP_MAX];
    int n = 0;
    if (bd_slot_ok(win_slot))
        for (int id = 0; id < LOGIT_BACKDROP_MAX; id++) {
            const struct wm_bd_reg *g = &g_bd[win_slot][id];
            if (!g->used) continue;
            struct drect o = { g->r.x0 + dx, g->r.y0 + dy, g->r.x1 + dx, g->r.y1 + dy };
            bd_and(&o, &c);
            if (!bd_empty(&o)) reg[n++] = o;
        }
    for (int y = c.y0; y < c.y1; y++) {
        const uint32_t *s = src->px + (long)(y - dy) * src->w;   /* s[x - dx] */
        uint32_t *row = t->px + (long)y * t->w;
        /* This row's region spans, sorted by start and merged: at most 8. */
        int sp0[LOGIT_BACKDROP_MAX], sp1[LOGIT_BACKDROP_MAX], m = 0;
        for (int k = 0; k < n; k++) {
            if (y < reg[k].y0 || y >= reg[k].y1) continue;
            int a = reg[k].x0, b = reg[k].x1, j = m++;
            while (j > 0 && sp0[j - 1] > a) { sp0[j] = sp0[j - 1]; sp1[j] = sp1[j - 1]; j--; }
            sp0[j] = a; sp1[j] = b;
        }
        int x = c.x0;
        for (int k = 0; k < m; k++) {
            if (sp1[k] <= x) continue;
            int a = sp0[k] > x ? sp0[k] : x, b = sp1[k];
            while (k + 1 < m && sp0[k + 1] <= b) { if (sp1[k + 1] > b) b = sp1[k + 1]; k++; }
            bd_copy(row + x, s + (x - dx), a - x);
            bd_copy_ink(row + a, s + (a - dx), b - a);
            x = b;
        }
        bd_copy(row + x, s + (x - dx), c.x1 - x);
    }
}
