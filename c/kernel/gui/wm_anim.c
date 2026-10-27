/* wm_anim.c -- window motion.
 *
 * Window motion: the per-frame clock every animation reads, the easing,
 * the open pop's scale, the dock fly (minimise / restore), the close fade's
 * ghosts, and each feature's part of the animation pass (the pass itself,
 * wm_anim_tick, and the damage contract, anim_damage, stay in wm.c).
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

/* ===========================================================================
 * MOTION -- the clock every animation reads.
 *
 * WHY MONOTONIC MILLISECONDS. An animation is elapsed time, so it uses the
 * clock that keeps advancing while interrupts are masked. timer_ticks() is an
 * IRQ0 delivery count: one long compositor/BKL hold can coalesce PIT edges and
 * leave that count unchanged. In production that once pinned an open pop at
 * p=0 for minutes, continuously redrawing the same frame and making the whole
 * desktop look hung. time_mono_ms() is TSC-backed when available, cannot run
 * backwards, and gives every animation a bounded wall-clock lifetime even
 * through a missed tick.
 *
 * NO FLOATS ANYWHERE. The kernel builds -msse2 now, but the easing is integer
 * because these curves are evaluated per window per frame and a fixed-point
 * quadratic is exact, reproducible, and diffable in a screenshot test. */

/* Long enough to read as motion rather than a cut, short
 * enough that it never becomes the thing you are waiting for. */
#define MINFLY_MS       180
#define OPEN_DUR_MS     160      /* the open pop, as it always was */

/* Animation timestamps use 0 as the inactive sentinel. Store start+1 so an
 * animation begun during the first monotonic millisecond is still active,
 * then decode before subtracting. The guarded subtraction also makes the
 * progress functions safe during an early clocksource rebase. */
uint64_t anim_stamp(void) { return time_mono_ms() + 1; }
uint64_t anim_elapsed_at(uint64_t stamp, uint64_t now)
{
    if (!stamp) return 0;
    uint64_t start = stamp - 1;
    return now >= start ? now - start : 0;
}
/* ONE INSTANT PER FRAME. While a frame is being composited, every animation
 * reads the time the frame began, not the time it happens to be drawn: a frame
 * is now drawn by several cores in bands (par_render), and a bouncing dock
 * icon or a ticking clock must not be at two different moments in one
 * picture. It is also what lets WM_PAR_VERIFY re-render a rectangle and expect
 * the same pixels. Outside a frame (input, hit tests) it is the live clock. */
uint64_t g_frame_ms;                   /* 0 = not inside a frame */
struct rtc_time g_frame_rtc;
static uint64_t anim_now(void) { return g_frame_ms ? g_frame_ms : time_mono_ms(); }
void frame_rtc(struct rtc_time *t) { if (g_frame_ms) *t = g_frame_rtc; else rtc_now(t); }
uint64_t anim_elapsed(uint64_t stamp)
{
    return anim_elapsed_at(stamp, anim_now());
}

/* Quadratic ease-out over 0..256: fast out of the gate, settling into the end.
 * Same curve family as gfx_shadow_falloff, and the same reason -- deceleration
 * is what makes a moving rectangle look like it has mass instead of being
 * teleported in equal steps.
 *
 * THE BODY MOVED TO c/lib/gfx (gfx_ease_out), and this is now a call, because
 * the widget toolkit needed the identical curve in ring 3. Two copies would
 * have been one jar with two doors on the two numbers a person sees side by
 * side: a control sliding inside a window while the window itself minimises.
 * c/lib/gfx is the only library both rings link, and wm.c already includes
 * gfx.h. Do not re-inline this; the duration constants above are the other
 * half of the same contract and are mirrored by AUI_T_BASE in aui.h. */
int ease_out(int t) { return ol_ease256(OL_EASE_OUT,t); }

/* ===========================================================================
 * THE DOCK FLY -- minimise and restore, as motion.
 *
 * WHAT THIS IS NOT, said here because it is the first thing anyone will look
 * for: it is NOT the genie. The genie warp bends a window around a curve by
 * resampling every COLUMN of it independently against a different vertical
 * scale and a different horizontal offset -- a per-column resample. This
 * compositor has no primitive for that and building one is a rasterizer, not
 * an animation: fb.c's scaled blits map a destination pixel to exactly one
 * source pixel through a single affine ratio, which is the wrong shape of
 * arithmetic entirely. What is built here is a scale-with-fade, which is what
 * macOS itself ships as the OTHER option in System Settings ("Minimise windows
 * using: Scale effect"), and calling it that is more honest than calling a
 * shrink a genie. If the warp is ever wanted, the thing to ask for is a
 * column-wise resample in c/lib/gfx or fb.c, not more code in this file.
 * ======================================================================== */

/* Progress of the dock fly, 0..256, where 0 is the window's own frame and 256
 * is its dock icon. Settled windows answer from `minimized` alone, which is
 * what makes this safe to call on every window every frame. */
int min_prog(const struct win *w)
{
    if (!w->min_t0 || wm_reduce_motion) return w->minimized ? 256 : 0;
    uint64_t e = anim_elapsed(w->min_t0);
    if (e >= MINFLY_MS) return w->min_dir ? 256 : 0;
    int eased = ease_out((int)(e * 256 / MINFLY_MS));
    return w->min_dir ? eased : 256 - eased;
}

/* The whole area a fly sweeps, end to end: the window's frame box unioned with
 * its dock icon box, both taken at the FOCUSED shadow extent. Used once, on the
 * frame the flight lands, to re-lay the entire path in a single rectangle --
 * see wm_anim_tick for why a per-frame union is not sufficient on its own. */
static void win_fly_sweep(const struct win *w, struct drect *r)
{
    int b = WSH_EXT(1), dy = 0;
    struct drect ic;
    dock_icon_box(w->min_slot, &ic);
    r->x0 = w->x - b;             r->y0 = w->y - b;
    r->x1 = w->x + w->w + b;      r->y1 = w->y + w->h + b + dy;
    if (ic.x0 - b < r->x0) r->x0 = ic.x0 - b;
    if (ic.y0 - b < r->y0) r->y0 = ic.y0 - b;
    if (ic.x1 + b > r->x1) r->x1 = ic.x1 + b;
    if (ic.y1 + b + dy > r->y1) r->y1 = ic.y1 + b + dy;
}

/* Minimise / restore. The state flips NOW and the picture takes MINFLY_MS
 * to agree -- see the comment on min_t0 in struct win. */
void win_set_min(struct win *w, int on)
{
    on = on ? 1 : 0;
    if (on == w->minimized && !w->min_t0) return;
    dirty_win(w);                    /* wherever it is at this instant */
    w->minimized = on;
    w->min_dir = on;
    w->min_slot = dock_slot_for(w);
    w->min_t0 = anim_stamp();
    if (!on) raise_win((int)(w - wm_wins));
    win_box(w, &w->anim_prev);       /* seed the per-frame damage union */
    dirty_win(w);
}

/* Put a window where the fly would have left it, immediately. Used when a
 * second gesture arrives on top of a running one: two animations moving the
 * same rectangle would each believe they own its position, and the damage
 * union would be computed against whichever of them wrote anim_prev last. */
void win_fly_settle(struct win *w)
{
    if (!w->min_t0) return;
    struct drect s;
    win_fly_sweep(w, &s);
    w->min_t0 = 0;
    dirty_rect(s.x0, s.y0, s.x1 - s.x0, s.y1 - s.y0);
}

/* ===========================================================================
 * THE CLOSE ANIMATION ("Fluid" P3): a closed window does not vanish in one
 * frame; its last picture shrinks to 0.92 and fades out over CLOSE_MS, the
 * way an opened one pops in. The window itself is gone the moment it closes --
 * its slot is free, focus has moved -- and what remains is a GHOST that owns
 * the window's last canvas and nothing else, so reusing the slot is safe.
 *
 * Drawn after the windows, as a solid titlebar strip plus the canvas, both
 * through anim_blit_fade/fb_blend (no shared scratch, so safe in a parallel
 * band). Damage is the window's original frame, which contains every shrunk
 * frame; the terminal tick damages it once more AFTER the ghost is gone, which
 * is what leaves no residue (-DWM_CLOSE_DAMAGE_LIE skips that last report and
 * make test-close-anim must then fail its round trip).
 * ======================================================================== */
#define CLOSE_MS 150
struct ghost g_ghost[NGHOST];
int g_nghost;

void ghost_box(const struct ghost *g, struct drect *r)
{ r->x0 = g->x; r->y0 = g->y; r->x1 = g->x + g->w; r->y1 = g->y + g->h; }

static int ghost_geom(const struct ghost *g, int *dx, int *dy, int *dw, int *dh, int *alpha)
{
    uint64_t e = anim_elapsed(g->t0);
    int p = e >= CLOSE_MS ? 256 : (int)(e * 256 / CLOSE_MS);
    int k = ease_out(p);
    int sc = 256 - k * 20 / 256;                      /* 1.00 -> 0.92 */
    *dw = g->w * sc / 256; *dh = g->h * sc / 256;
    *dx = g->x + (g->w - *dw) / 2; *dy = g->y + (g->h - *dh) / 2;
    *alpha = 255 - 255 * k / 256;
#ifdef WM_CLOSE_DAMAGE_LIE
    /* The control must leave something for the missing damage to strand. The
     * quadratic ease reaches k = 256 at p >= 241, so a last tick that lands in
     * the final 6% of CLOSE_MS draws alpha 0 -- nothing -- and the lie passed
     * the round trip (2026-10-26: TextEdit's fitted frame changed the frame
     * timing and the control ended at p 242). A floor makes every drawn frame
     * visible, so the control fails whatever tick ends the fade. */
    if (*alpha < 64) *alpha = 64;
#endif
    return p;
}

void draw_ghost(const struct wm_target *t, const struct drect *R, const struct ghost *g)
{
    int dx, dy, dw, dh, a;
    ghost_geom(g, &dx, &dy, &dw, &dh, &a);
    if (a <= 0 || dw <= 0 || dh <= 0) return;
    int tbh = g->tbh * dw / (g->w ? g->w : 1);
    if (tbh > dh) tbh = dh;
    if (tbh > 0) {
        if (g->dark) fb_blend_round_rect(dx, dy, dw, tbh + S(10), S(10), 44, 44, 52, a);
        else         fb_blend_round_rect(dx, dy, dw, tbh + S(10), S(10), 236, 236, 240, a);
    }
    if (g->surf.px && dh - tbh > 0) anim_blit_fade(t, R, dx, dy + tbh, dw, dh - tbh, &g->surf, a);
}

/* Take window wi's canvas for a ghost. 1 if taken (the caller must not free
 * it), 0 if this close should just vanish (reduce motion, locked screen, a
 * window not drawn the ordinary way, or no free ghost). */
int ghost_take(int wi)
{
    struct win *w = &wm_wins[wi];
    int x, y, ww, wh, a;
    if (wm_reduce_motion || g_locked || w->minimized || !w->surf.px) return 0;
    if (!win_draw_rect(w, &x, &y, &ww, &wh, &a) || !win_drawn_direct(w, x, y, ww, wh, a)) return 0;
    for (int k = 0; k < NGHOST; k++) {
        struct ghost *g = &g_ghost[k];
        if (g->used) continue;
        g->surf = w->surf;
        g->x = w->x; g->y = w->y; g->w = w->w; g->h = w->h;
        g->dark = g_ui_dark;
        g->tbh = win_tbh(w);
        g->t0 = anim_stamp();
        g->used = 1;
        g_nghost++;
        return 1;
    }
    return 0;
}
/* Open "pop" animation: 0.85 -> 1.0 scale over ~0.14s (easeOutCubic). Returns the
 * scale in /256 (256 = full), or 0 when settled / not animating.
 *
 * PURE NOW -- it used to clear open_t0 on the frame it expired, which made
 * "where is this window" a question that changed the answer to the next one.
 * wm_anim_tick() owns every timer's end; see the note on win_draw_rect. */
int win_open_scale(const struct win *w)
{
    if (!w->open_t0 || wm_reduce_motion) return 0;
    uint64_t e = anim_elapsed(w->open_t0);
    if (e >= OPEN_DUR_MS) return 0;
    int eased = ol_ease256(OL_EASE_OUT_CUBIC, (int)(e * 256 / OPEN_DUR_MS));
    return 216 + (256 - 216) * eased / 256;            /* 0.84x -> 1.0x over ~0.16s */
}

void anim_trace(const char *what, int wi, const struct win *w, int p)
{
    int x, y, ww, wh, a;
    if (!win_draw_rect(w, &x, &y, &ww, &wh, &a)) return;
    kprintf("[wm] anim %s win %d p %d rect %d %d %d %d alpha %d home %d %d %d %d"
            " damage from %d %d %d %d cur %d %d %d %d sent %d %d %d %d\n",
            what, wi, p, x, y, ww, wh, a, w->x, w->y, w->w, w->h,
            w->anim_from.x0, w->anim_from.y0, w->anim_from.x1, w->anim_from.y1,
            w->anim_prev.x0, w->anim_prev.y0, w->anim_prev.x1, w->anim_prev.y1,
            w->anim_sent.x0, w->anim_sent.y0, w->anim_sent.x1, w->anim_sent.y1);
}

/* 1. THE OPEN POP -- the first frame is full because adding a new topmost
 *    window changes focus and stacking. Every frame after that is only
 *    (where the scaled window was | where it is), the same damage contract
 *    minimise and Expose use below. The old path forced all 2,304,000
 *    pixels at 1920x1200 for every tick of a 0.84x -> 1.0x window whose
 *    complete swept box is under half that area. That spent most of the
 *    animation redrawing untouched wallpaper, menu bar and dock, reducing
 *    the number of distinct positions that could land before 160 ms elapsed.
 *
 * Clearing open_t0 BEFORE anim_damage() is the terminal-frame contract:
 * win_box() then returns the ordinary full-size frame and unions it with
 * anim_prev, so the exact endpoint is painted once and no near-final scale
 * is left on screen. The growing rectangles are nested, but the union keeps
 * this correct if the curve or anchor changes later. */
void wm_anim_open_tick(uint64_t t)
{
    for (int i = 0; i < MAXWIN; i++) {
        struct win *w = &wm_wins[i];
        if (!w->used || !w->open_t0) continue;
        uint64_t e = anim_elapsed_at(w->open_t0, t);
        if (wm_reduce_motion || e >= OPEN_DUR_MS) w->open_t0 = 0;
        anim_damage(w);
        if (w->open_t0)
            anim_trace("open", i, w, (int)(e * 256 / OPEN_DUR_MS));
    }
}

/* 1b. THE CLOSE FADE. Damage the whole original frame every tick; on the
 *     last one the ghost is freed FIRST and then damaged, so the frame
 *     that follows paints what was under it. */
void wm_anim_close_tick(uint64_t t)
{
    for (int k = 0; g_nghost && k < NGHOST; k++) {
        struct ghost *g = &g_ghost[k];
        if (!g->used) continue;
        struct drect b; ghost_box(g, &b);
        uint64_t e = anim_elapsed_at(g->t0, t);
        if (e >= CLOSE_MS || wm_reduce_motion) {
            if (g->surf.px) kfree(g->surf.px);
            g->surf.px = 0;
            g->used = 0;
            g_nghost--;
            kprintf("[wm] anim close ghost %d done\n", k);
#ifndef WM_CLOSE_DAMAGE_LIE
            dirty_rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
#endif
            continue;
        }
        kprintf("[wm] anim close ghost %d p %d\n", k, (int)(e * 256 / CLOSE_MS));
        dirty_rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    }
}

/* 2. THE DOCK FLY. The landing frame re-lays the whole sweep once
 *    (wm_anim_sweep_damage, wm.c), then the dock: a running dot's window
 *    arrived, or left. */
void wm_anim_fly_tick(uint64_t t)
{
    for (int i = 0; i < MAXWIN; i++) {
        struct win *w = &wm_wins[i];
        if (!w->used || !w->min_t0) continue;
        if (wm_reduce_motion || anim_elapsed_at(w->min_t0, t) >= MINFLY_MS) {
            struct drect s;
            win_fly_sweep(w, &s);
            w->min_t0 = 0;
            wm_anim_sweep_damage(&s);
            dirty_dock();          /* a running dot's window arrived, or left */
            continue;
        }
        anim_damage(w);
        anim_trace(w->min_dir ? "min" : "restore", i, w, min_prog(w));
    }
}
