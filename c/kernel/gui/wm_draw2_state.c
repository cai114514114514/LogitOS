/* Clip and group stacks, BLIT2 (GUI ABI v2): SYS_GUI_CLIP_PUSH/POP,
 * SYS_GUI_GROUP_BEGIN/END and SYS_GUI_BLIT2 over per-window state.
 *
 * STATE. g_w2[slot] (slot = wm.c's window slot; wm.c asserts MAXWIN ==
 * WM2_MAX_WINDOWS) holds a 16-deep stack of open levels and a snapshot
 * arena. A level is one of
 *   - a SQUARE CLIP: e->clip_push (fb_clip_push) and nothing else;
 *   - a ROUNDED CLIP: e->clip_push of the group's effective rect, then
 *     ol_display_group_begin at opacity 255 (corner tiles in the arena);
 *   - a GROUP: the same with the group's opacity (full snapshot in the arena).
 * Levels span syscalls, and fb_sdk() rebinds the display -- clip included --
 * from the surface on every call, so the clip always goes through the surface
 * (fb-clip-stack). The rect pushed for a rounded clip or a group is exactly
 * the clip ol_display_group_begin installs on the display (the group rect
 * intersected with the target and the clip in force, empty made canonical,
 * as display_group.c's dg_effective), so after the next rebind the display
 * clip is still the one group_end expects, and the children are clipped to it.
 *
 * THE ARENA is one buffer per window, allocated lazily through e->alloc and
 * grown by doubling (open snapshots are copied and their pointers rebased),
 * never beyond min(2 x the window surface bytes, 32 MiB). Levels take their
 * snapshot at the watermark and give it back when they close (LIFO).
 * OL_LIMIT (arena or depth exhausted, or the allocation failed) changes
 * nothing: the stack is intact and AUI falls back to its faux fade.
 *
 * FLUSH (wm2_state_win_flush, from the router's wm2_win_flush on
 * SYS_GUI_FLUSH/FLUSH_RECT) closes whatever the frame left open, innermost
 * first, counts each such level in the window's error counter
 * (wm2_state_err_count, SYS_GUI2_INFO(1)) and resets the arena watermark.
 * RESET (wm2_state_win_reset: create, destroy, resize) frees the arena and
 * forgets the levels; their pixels belong to the old canvas.
 *
 * THE FB CLIP LEDGER. fb_clip_push keeps the clip each push replaced on a
 * stack PER DRAWING CONTEXT, not per surface, and every GUI syscall draws on
 * context 0; this module is its only user (wm.c's wm2_clip_push_fb). Because
 * levels span syscalls, two windows' pushes can interleave on that one stack.
 * g_fbl[] mirrors it (owner slot, level, and the surface clip the push
 * replaced), so:
 *   - a pop whose push is not on top is still exact when every push above it
 *     replaced the SAME clip (the common case: two windows, each one level
 *     deep over an unclipped canvas -- fb_clip_pop restores the top's value,
 *     which is then this level's value); otherwise the pop answers OL_STATE
 *     and changes nothing (a flush then closes the group part and orphans the
 *     push);
 *   - a reset orphans the slot's pushes (there is no env to pop with, and the
 *     surface is gone or reinitialised). Orphans on top of the stack are
 *     reclaimed by the next state call of ANY window whose current clip equals
 *     the clip the bottom orphan replaced -- popping them then leaves that
 *     window's clip unchanged.
 * fb_set_backbuffer resets fb's stack; it runs only at mode set, with no
 * window drawing, and the ledger is never consulted against it.
 *
 * BLIT2 streams the source: the visible destination rows are cut into bands,
 * each band's source rows (the engine's own tap footprint, plus one row of
 * margin) are copied from user memory into a tile in
 * display_scratch(OL_DS_USER) -- 256 KiB per band when the scale allows,
 * never more than 4 MiB -- and ol_display_blit_rgba_ex draws the band with the
 * display clip narrowed to it. Every band has at least two visible rows (or is
 * the whole visible set), which keeps the engine's two-tap/general-path
 * choice, and so its output, bit-identical to one unbanded call. A downscale
 * whose two-row band still needs more than 4 MiB answers OL_LIMIT.
 *
 * COORDINATES. Rects convert through wm2_rect24 (LOGIT_RECT_SNAP honoured);
 * the clip/group rect is the nearest device pixel of each converted edge
 * (exact for SNAP, where edges are already whole). Radii are wm2_pt24 rounded
 * to the nearest device pixel; ol_display_group_begin clamps them to
 * min(w,h)/2.
 *
 * LOCKING. g_w2[] and g_fbl[] are kernel-only state outside c/lib/gfx. They
 * are mutated only by GUI syscalls -- which draw on drawing context 0 with
 * graphics_lock held (wm.c, wm_gui_dispatch) -- and by the wm2_win_reset
 * hooks under the same lock (reap() runs inside wm_render_locked). The
 * compositor never reads them. graphics_lock is a sleeping mutex, so e->alloc
 * and e->copyin may block here. Blit tiles live in the per-context display
 * scratch, so nothing here needs GFX_NCTX copies.
 *
 * NEGATIVE CONTROL: -DWM_DRAW2_CLIP_DISPLAY_ONLY sets the clip on e->d only,
 * never through e->clip_push -- a rebind then forgets it
 * (tests/unit/wm_draw2_state_test.c, 'FAIL rebind-clip'). */
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "openlogit_display_group.h"
#include "openlogit_display_blit.h"
#include "display_pixels.h"

const unsigned wm2_state_info = LOGIT_GUI2_F_CLIP | LOGIT_GUI2_F_GROUP | LOGIT_GUI2_F_BLIT2;

#define WM2_DEPTH        16
#define WM2_FB_DEPTH     16                 /* fb.c FB_CLIP_DEPTH */
#define WM2_ARENA_CAP    (32UL << 20)
#define WM2_ARENA_MIN    (64UL << 10)
#define WM2_TILE_TARGET  (256UL << 10)
#define WM2_TILE_CAP     (4UL << 20)

enum { WM2_LV_CLIP = 1, WM2_LV_RCLIP, WM2_LV_GROUP };

struct wm2_clipv { int on, x0, y0, x1, y1; };

struct wm2_level {
    int kind;
    int fb;                    /* g_fbl index of this level's push, or -1 */
    struct wm2_clipv saved;    /* the display (= surface) clip before the push */
    unsigned long off, bytes;  /* snapshot in the arena */
    struct ol_display_group g;
};

struct wm2_win {
    int depth;
    struct wm2_level lv[WM2_DEPTH];
    uint32_t *arena;
    unsigned long arena_n, used;
    void (*release)(void *);
    unsigned err;
};
static struct wm2_win g_w2[WM2_MAX_WINDOWS];

struct wm2_fbent { int slot, level, orphan; struct wm2_clipv saved; };
static struct wm2_fbent g_fbl[WM2_FB_DEPTH];
static int g_fbn;

/* ---- small helpers ------------------------------------------------------- */

static int wm2_round_px(long long v24) { return wm2_sat(wm2_floordiv(v24 + 128, 256)); }

static struct wm2_clipv wm2_cur_clip(const struct wm2_env *e)
{
    const struct ol_pixel_target *t = &e->d->target;
    struct wm2_clipv c = { t->clip_on, t->clx0, t->cly0, t->clx1, t->cly1 };
    return c;
}

static void wm2_set_clip(struct wm2_env *e, struct wm2_clipv c)
{
    struct ol_pixel_target *t = &e->d->target;
    t->clip_on = c.on; t->clx0 = c.x0; t->cly0 = c.y0; t->clx1 = c.x1; t->cly1 = c.y1;
}

/* Clips that restore identically: every "off" is the same clip. */
static int wm2_clip_eq(struct wm2_clipv a, struct wm2_clipv b)
{
    if (!a.on || !b.on) return !a.on && !b.on;
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

#ifdef WM_DRAW2_CLIP_DISPLAY_ONLY
/* fb_clip_push's intersection (set_clip semantics, empty stays empty). */
static struct wm2_clipv wm2_clip_isect(struct wm2_clipv c, int x0, int y0, int x1, int y1)
{
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    if (c.on) {
        if (x0 < c.x0) x0 = c.x0;
        if (y0 < c.y0) y0 = c.y0;
        if (x1 > c.x1) x1 = c.x1;
        if (y1 > c.y1) y1 = c.y1;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    struct wm2_clipv r = { 1, x0, y0, x1, y1 };
    return r;
}
#endif

static int wm2_slot_ok(const struct wm2_env *e)
{
    return e && e->d && e->slot >= 0 && e->slot < WM2_MAX_WINDOWS;
}

/* ---- the fb clip ledger -------------------------------------------------- */

/* Pop the orphans on top of fb's stack when that leaves this window's clip
 * as it is (see THE FB CLIP LEDGER). */
static void wm2_reclaim(struct wm2_env *e, struct wm2_clipv now)
{
    int k = g_fbn;
    while (k > 0 && g_fbl[k - 1].orphan) k--;
    if (k == g_fbn || !e->clip_pop || !wm2_clip_eq(g_fbl[k].saved, now)) return;
    while (g_fbn > k) { e->clip_pop(e->opaque); g_fbn--; }
}

static long wm2_fb_push(struct wm2_env *e, struct wm2_level *lv, int level,
                        struct wm2_clipv r)
{
    lv->fb = -1;
#ifdef WM_DRAW2_CLIP_DISPLAY_ONLY
    (void)level;
    if (lv->kind == WM2_LV_CLIP) wm2_set_clip(e, wm2_clip_isect(lv->saved, r.x0, r.y0, r.x1, r.y1));
    return OL_OK;
#else
    if (g_fbn >= WM2_FB_DEPTH) return OL_LIMIT;
    if (!e->clip_push) return OL_UNSUPPORTED;
    int rc = e->clip_push(e->opaque, r.x0, r.y0, r.x1, r.y1);
    if (rc != OL_OK) return rc;
    struct wm2_fbent *f = &g_fbl[g_fbn];
    f->slot = e->slot; f->level = level; f->orphan = 0; f->saved = lv->saved;
    lv->fb = g_fbn++;
    return OL_OK;
#endif
}

/* Can this level's push be popped exactly now? */
static int wm2_fb_can_pop(const struct wm2_level *lv)
{
    if (lv->fb < 0) return 1;
    for (int i = lv->fb + 1; i < g_fbn; i++)
        if (!wm2_clip_eq(g_fbl[i].saved, g_fbl[lv->fb].saved)) return 0;
    return 1;
}

static void wm2_fb_pop(struct wm2_env *e, struct wm2_level *lv)
{
    int k = lv->fb;
    if (k < 0) return;
    if (e->clip_pop) e->clip_pop(e->opaque);
    for (int i = k; i < g_fbn - 1; i++) {
        g_fbl[i] = g_fbl[i + 1];
        if (!g_fbl[i].orphan) g_w2[g_fbl[i].slot].lv[g_fbl[i].level].fb = i;
    }
    g_fbn--;
    lv->fb = -1;
}

/* ---- the snapshot arena -------------------------------------------------- */

static unsigned long wm2_arena_cap(const struct wm2_env *e)
{
    if (e->surf_w <= 0 || e->surf_h <= 0) return 0;
    unsigned long s = (unsigned long)e->surf_w * (unsigned long)e->surf_h * 8UL;
    return s > WM2_ARENA_CAP ? WM2_ARENA_CAP : s;
}

static long wm2_arena_reserve(struct wm2_env *e, struct wm2_win *w, unsigned long need)
{
    if (!need) return OL_OK;
    unsigned long cap = wm2_arena_cap(e);
    if (need > cap || w->used > cap - need) return OL_LIMIT;
    unsigned long want = w->used + need;
    if (want <= w->arena_n) return OL_OK;
    unsigned long n = w->arena_n ? w->arena_n : WM2_ARENA_MIN;
    while (n < want) n *= 2;
    if (n > cap) n = cap;
    if (!e->alloc) return OL_LIMIT;
    uint32_t *nb = (uint32_t *)e->alloc(n);
    if (!nb) return OL_LIMIT;
    for (unsigned long i = 0; i < w->used / 4; i++) nb[i] = w->arena[i];
    for (int k = 0; k < w->depth; k++)
        if (w->lv[k].bytes) w->lv[k].g.snap = nb + w->lv[k].off / 4;
    if (w->arena && w->release) w->release(w->arena);
    w->arena = nb;
    w->arena_n = n;
    w->release = e->release;
    return OL_OK;
}

/* ---- opening and closing levels ------------------------------------------ */

static long wm2_open_clip(struct wm2_env *e, struct wm2_win *w, struct wm2_clipv now,
                          int x0, int y0, int x1, int y1)
{
    struct wm2_level *lv = &w->lv[w->depth];
    lv->kind = WM2_LV_CLIP;
    lv->saved = now;
    lv->off = w->used;
    lv->bytes = 0;
    lv->g.flags = 0;
    struct wm2_clipv r = { 1, x0, y0, x1, y1 };
    long rc = wm2_fb_push(e, lv, w->depth, r);
    if (rc != OL_OK) return rc;
    w->depth++;
    return OL_OK;
}

static long wm2_open_group(struct wm2_env *e, struct wm2_win *w, int kind,
                           struct wm2_clipv now, int x, int y, int gw, int gh,
                           const int rad[4], int opacity)
{
    unsigned long need = ol_display_group_bytes(gw, gh, rad, opacity);
    long rc = wm2_arena_reserve(e, w, need);
    if (rc != OL_OK) return rc;
    struct wm2_level *lv = &w->lv[w->depth];
    lv->kind = kind;
    lv->saved = now;
    lv->off = w->used;
    lv->bytes = need;
    /* The clip group_begin will install (display_group.c dg_effective). */
    const struct ol_pixel_target *t = &e->d->target;
    long long ex0 = x, ey0 = y, ex1 = (long long)x + gw, ey1 = (long long)y + gh;
    if (ex0 < 0) ex0 = 0;
    if (ey0 < 0) ey0 = 0;
    if (ex1 > t->w) ex1 = t->w;
    if (ey1 > t->h) ey1 = t->h;
    if (now.on) {
        if (now.x0 > ex0) ex0 = now.x0;
        if (now.y0 > ey0) ey0 = now.y0;
        if (now.x1 < ex1) ex1 = now.x1;
        if (now.y1 < ey1) ey1 = now.y1;
    }
    if (ex1 <= ex0 || ey1 <= ey0) { ex1 = ex0; ey1 = ey0; }
    struct wm2_clipv r = { 1, wm2_sat(ex0), wm2_sat(ey0), wm2_sat(ex1), wm2_sat(ey1) };
    rc = wm2_fb_push(e, lv, w->depth, r);
    if (rc != OL_OK) return rc;
    rc = ol_display_group_begin(e->d, &lv->g, x, y, gw, gh, rad, opacity,
                                need ? w->arena + w->used / 4 : 0, need);
    if (rc != OL_OK) {
        wm2_fb_pop(e, lv);
        wm2_set_clip(e, now);
        return rc;
    }
    w->used += need;
    w->depth++;
    return OL_OK;
}

/* Close the top level. The caller has checked wm2_fb_can_pop, or orphaned
 * the push. Leaves e->d's clip equal to what the surface clip now is, so a
 * further close in the same call sees a coherent display. */
static long wm2_close_top(struct wm2_env *e, struct wm2_win *w)
{
    struct wm2_level *lv = &w->lv[w->depth - 1];
    long st = OL_OK;
    if (lv->kind != WM2_LV_CLIP) st = ol_display_group_end(e->d, &lv->g);
    wm2_fb_pop(e, lv);
    wm2_set_clip(e, lv->saved);
    w->used = lv->off;
    w->depth--;
    return st;
}

/* Common entry for the stack calls: the window, after reclaiming orphans. */
static struct wm2_win *wm2_enter(struct wm2_env *e, struct wm2_clipv *now)
{
    *now = wm2_cur_clip(e);
    wm2_reclaim(e, *now);
    return &g_w2[e->slot];
}

static long wm2_pop_kind(struct wm2_env *e, int group)
{
    if (!wm2_slot_ok(e)) return OL_STATE;
    struct wm2_clipv now;
    struct wm2_win *w = wm2_enter(e, &now);
    if (w->depth <= 0) return OL_STATE;
    struct wm2_level *lv = &w->lv[w->depth - 1];
    if ((lv->kind == WM2_LV_GROUP) != group) return OL_STATE;
    if (!wm2_fb_can_pop(lv)) return OL_STATE;
    return wm2_close_top(e, w);
}

/* Device geometry of a clip/group: nearest pixel of each converted edge. */
static long wm2_geom(const struct wm2_env *e, int x, int y, int w, int h,
                     const int radius[4], unsigned flags, int out[4], int rad[4])
{
    if (flags & ~LOGIT_RECT_SNAP) return OL_ARGUMENT;
    if (w < 0 || h < 0) return OL_ARGUMENT;
    for (int k = 0; k < 4; k++) {
        if (radius[k] < 0) return OL_ARGUMENT;
        rad[k] = wm2_round_px(wm2_pt24(radius[k], e->scale_pct));
    }
    struct wm2_rect24 r = wm2_rect24(x, y, w, h, e->scale_pct, (flags & LOGIT_RECT_SNAP) != 0);
    int x0 = wm2_round_px(r.x), y0 = wm2_round_px(r.y);
    int x1 = wm2_round_px((long long)r.x + r.w), y1 = wm2_round_px((long long)r.y + r.h);
    out[0] = x0; out[1] = y0;
    out[2] = wm2_sat((long long)x1 - x0); out[3] = wm2_sat((long long)y1 - y0);
    return OL_OK;
}

/* ---- the handlers -------------------------------------------------------- */

long wm2_clip_push(struct wm2_env *e, long a, long b, long c)
{
    struct logit_clip2 cl;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &cl, a, sizeof cl);
    if (rc != OL_OK) return rc;
    if (!wm2_slot_ok(e)) return OL_STATE;
    int g[4], rad[4];
    rc = wm2_geom(e, cl.x, cl.y, cl.w, cl.h, cl.radius, cl.flags, g, rad);
    if (rc != OL_OK) return rc;
    struct wm2_clipv now;
    struct wm2_win *w = wm2_enter(e, &now);
    if (w->depth >= WM2_DEPTH) return OL_LIMIT;
    if (rad[0] | rad[1] | rad[2] | rad[3])
        return wm2_open_group(e, w, WM2_LV_RCLIP, now, g[0], g[1], g[2], g[3], rad, 255);
    return wm2_open_clip(e, w, now, g[0], g[1], wm2_sat((long long)g[0] + g[2]),
                         wm2_sat((long long)g[1] + g[3]));
}

long wm2_clip_pop(struct wm2_env *e, long a, long b, long c)
{
    (void)a; (void)b; (void)c;
    return wm2_pop_kind(e, 0);
}

long wm2_group_begin(struct wm2_env *e, long a, long b, long c)
{
    struct logit_group2 gr;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &gr, a, sizeof gr);
    if (rc != OL_OK) return rc;
    if (!wm2_slot_ok(e)) return OL_STATE;
    if (gr.opacity < 0 || gr.opacity > 255) return OL_ARGUMENT;
    int g[4], rad[4];
    rc = wm2_geom(e, gr.x, gr.y, gr.w, gr.h, gr.radius, gr.flags, g, rad);
    if (rc != OL_OK) return rc;
    struct wm2_clipv now;
    struct wm2_win *w = wm2_enter(e, &now);
    if (w->depth >= WM2_DEPTH) return OL_LIMIT;
    return wm2_open_group(e, w, WM2_LV_GROUP, now, g[0], g[1], g[2], g[3], rad, gr.opacity);
}

long wm2_group_end(struct wm2_env *e, long a, long b, long c)
{
    (void)a; (void)b; (void)c;
    return wm2_pop_kind(e, 1);
}

/* ---- BLIT2 --------------------------------------------------------------- */

struct wm2_blit {
    struct wm2_env *e;
    uintptr_t rgba;            /* user pointer of the sub-image's first pixel */
    long long sstride;
    int src_w, src_h, opacity, filter, premul;
    int dx24, dy24, dw24, dh24;
    int vx0, vx1;              /* visible device columns */
};

/* Source rows (sub-image relative) the engine can read for destination rows
 * [p0, p1): bx_axis's mapping, one row of margin each side for the bilinear
 * centre shift, clamped to the sub-image. */
static void wm2_blit_rows(const struct wm2_blit *b, int p0, int p1, int *k0, int *k1)
{
    long long org = b->dy24, len = b->dh24, slen = b->src_h;
    long long lo = (long long)p0 * 256, hi = (long long)p1 * 256;
    if (lo < org) lo = org;
    if (hi > org + len) hi = org + len;
    long long sa = ((lo - org) * slen * 65536) / len;
    long long sb = ((hi - org) * slen * 65536) / len;
    long long r0 = (sa >> 16) - 1, r1 = (sb >> 16) + 1;
    if (r0 < 0) r0 = 0;
    if (r1 > slen - 1) r1 = slen - 1;
    *k0 = (int)r0;
    *k1 = (int)r1;
}

static unsigned long wm2_band_bytes(const struct wm2_blit *b, int p0, int p1)
{
    int k0, k1;
    wm2_blit_rows(b, p0, p1, &k0, &k1);
    return (unsigned long)(k1 - k0 + 1) * (unsigned long)b->src_w * 4UL;
}

static void wm2_unpremul(uint8_t *p, int n)
{
    for (int i = 0; i < n; i++, p += 4) {
        unsigned a = p[3];
        if (a == 255) continue;
        if (!a) { p[0] = p[1] = p[2] = 0; continue; }
        for (int q = 0; q < 3; q++) {
            unsigned v = (p[q] * 255u + a / 2) / a;
            p[q] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

static long wm2_blit_band(const struct wm2_blit *b, int p0, int p1)
{
    struct ol_display *d = b->e->d;
    int k0, k1;
    wm2_blit_rows(b, p0, p1, &k0, &k1);
    unsigned long rowb = (unsigned long)b->src_w * 4UL;
    unsigned long bytes = (unsigned long)(k1 - k0 + 1) * rowb;
    if (bytes > WM2_TILE_CAP) return OL_LIMIT;
    uint8_t *tile = (uint8_t *)display_scratch(d, OL_DS_USER, bytes);
    if (!tile) return OL_LIMIT;
    for (int r = k0; r <= k1; r++) {
        uint8_t *dst = tile + (unsigned long)(r - k0) * rowb;
        uintptr_t src = b->rgba + (uintptr_t)((unsigned long long)r * (unsigned long long)b->sstride);
        if (b->e->copyin(dst, (const void *)src, rowb) < 0) return OL_ARGUMENT;
        if (b->premul) wm2_unpremul(dst, b->src_w);
    }
    struct wm2_clipv keep = wm2_cur_clip(b->e);
    struct wm2_clipv band = { 1, b->vx0, p0, b->vx1, p1 };
    wm2_set_clip(b->e, band);
    /* The tile holds sub-image rows k0..k1; present it to the engine as the
     * whole sub-image, addressed so that row r lands on tile row r - k0. The
     * engine reads only rows of the visible band, all inside the tile. */
    const uint8_t *base = (const uint8_t *)((uintptr_t)tile - (uintptr_t)k0 * rowb);
    int rc = ol_display_blit_rgba_ex(d, base, b->src_w, b->src_h, (int)rowb,
                                     0, 0, b->src_w, b->src_h,
                                     b->dx24, b->dy24, b->dw24, b->dh24,
                                     b->opacity, (enum ol_filter)b->filter);
    wm2_set_clip(b->e, keep);
    return rc;
}

long wm2_blit2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_blit2 bl;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &bl, a, sizeof bl);
    if (rc != OL_OK) return rc;
    if (!wm2_slot_ok(e)) return OL_STATE;
    if (bl.flags & ~(LOGIT_RECT_SNAP | LOGIT_BLIT2_PREMUL)) return OL_ARGUMENT;
    if (bl.filter < LOGIT_FILTER_NEAREST || bl.filter > LOGIT_FILTER_BOX) return OL_ARGUMENT;
    if (!bl.rgba || bl.sw <= 0 || bl.sh <= 0 || bl.sw > 32768 || bl.sh > 32768 ||
        bl.sstride < bl.sw * 4 || bl.src_x < 0 || bl.src_y < 0 || bl.src_w <= 0 ||
        bl.src_h <= 0 || bl.src_x > bl.sw - bl.src_w || bl.src_y > bl.sh - bl.src_h)
        return OL_ARGUMENT;
    if (bl.w < 0 || bl.h < 0 || bl.opacity < 0 || bl.opacity > 255) return OL_ARGUMENT;
    _Static_assert(LOGIT_FILTER_NEAREST == OL_FILTER_NEAREST &&
                   LOGIT_FILTER_BILINEAR == OL_FILTER_BILINEAR &&
                   LOGIT_FILTER_BOX == OL_FILTER_BOX, "ABI filter numbers = enum ol_filter");
    struct wm2_rect24 r = wm2_rect24(bl.x, bl.y, bl.w, bl.h, e->scale_pct,
                                     (bl.flags & LOGIT_RECT_SNAP) != 0);
    if (r.w <= 0 || r.h <= 0 || bl.opacity == 0) return OL_OK;
    const struct ol_pixel_target *t = &e->d->target;
    if (!t->px) return OL_STATE;

    /* Visible device rect: the engine's X0..X1 / Y0..Y1, target, clip. */
    long long X0 = (long long)r.x >> 8, X1 = ((long long)r.x + r.w + 255) >> 8;
    long long Y0 = (long long)r.y >> 8, Y1 = ((long long)r.y + r.h + 255) >> 8;
    long long cx0 = 0, cy0 = 0, cx1 = t->w, cy1 = t->h;
    if (t->clip_on) {
        if (t->clx0 > cx0) cx0 = t->clx0;
        if (t->cly0 > cy0) cy0 = t->cly0;
        if (t->clx1 < cx1) cx1 = t->clx1;
        if (t->cly1 < cy1) cy1 = t->cly1;
    }
    if (X0 < cx0) X0 = cx0;
    if (Y0 < cy0) Y0 = cy0;
    if (X1 > cx1) X1 = cx1;
    if (Y1 > cy1) Y1 = cy1;
    if (X1 <= X0 || Y1 <= Y0) return OL_OK;

    struct wm2_blit s;
    s.e = e;
    s.sstride = bl.sstride;
    s.rgba = (uintptr_t)bl.rgba + (uintptr_t)((unsigned long long)bl.src_y * (unsigned long long)bl.sstride)
             + (uintptr_t)bl.src_x * 4u;
    s.src_w = bl.src_w; s.src_h = bl.src_h;
    s.opacity = bl.opacity; s.filter = bl.filter;
    s.premul = (bl.flags & LOGIT_BLIT2_PREMUL) != 0;
    s.dx24 = r.x; s.dy24 = r.y; s.dw24 = r.w; s.dh24 = r.h;
    s.vx0 = (int)X0; s.vx1 = (int)X1;

    /* Band height: about WM2_TILE_TARGET of source rows, at least 2 rows, and
     * never a 1-row tail (see BLIT2 above). */
    unsigned long rowb = (unsigned long)bl.src_w * 4UL;
    unsigned long per = (unsigned long)(((long long)bl.src_h * 256 + r.h - 1) / r.h) + 1;
    unsigned long guess = WM2_TILE_TARGET / (rowb * per);
    int p = (int)Y0, end = (int)Y1;
    while (p < end) {
        int rem = end - p;
        int band = guess > (unsigned long)rem ? rem : (int)guess;
        for (;;) {
            if (band < 2) band = 2;
            if (band >= rem) band = rem;
            else if (rem - band == 1) band = band > 2 ? band - 1 : band + 1;
            if (wm2_band_bytes(&s, p, p + band) <= WM2_TILE_CAP) break;
            int least = rem <= 3 ? rem : 2;
            if (band <= least) return OL_LIMIT;
            band /= 2;
        }
        rc = wm2_blit_band(&s, p, p + band);
        if (rc != OL_OK) return rc;
        p += band;
    }
    return OL_OK;
}

/* ---- router hooks -------------------------------------------------------- */

void wm2_state_win_reset(int slot)
{
    if (slot < 0 || slot >= WM2_MAX_WINDOWS) return;
    struct wm2_win *w = &g_w2[slot];
    for (int k = 0; k < w->depth; k++)
        if (w->lv[k].fb >= 0) g_fbl[w->lv[k].fb].orphan = 1;
    if (w->arena && w->release) w->release(w->arena);
    w->arena = 0;
    w->arena_n = 0;
    w->used = 0;
    w->release = 0;
    w->depth = 0;
    w->err = 0;
}

void wm2_state_win_flush(struct wm2_env *e)
{
    if (!wm2_slot_ok(e)) return;
    struct wm2_clipv now;
    struct wm2_win *w = wm2_enter(e, &now);
    while (w->depth > 0) {
        struct wm2_level *lv = &w->lv[w->depth - 1];
        if (!wm2_fb_can_pop(lv)) {        /* see THE FB CLIP LEDGER */
            g_fbl[lv->fb].orphan = 1;
            lv->fb = -1;
        }
        (void)wm2_close_top(e, w);
        if (w->err != ~0u) w->err++;
    }
    w->used = 0;
    wm2_reclaim(e, wm2_cur_clip(e));
}

unsigned wm2_state_err_count(int slot)
{
    if (slot < 0 || slot >= WM2_MAX_WINDOWS) return 0;
    return g_w2[slot].err;
}
