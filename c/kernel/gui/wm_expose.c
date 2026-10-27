/* wm_expose.c -- Exposé.
 *
 * Exposé, the all-windows picker: its state, the grid, the transition's
 * progress, entering and leaving, the hot corner, its share of the pointer
 * and its part of the animation pass.
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

#define EX_DUR_MS       180      /* the transition, as long as the dock fly (MINFLY_MS) */
#define EX_GUTTER_PT    26      /* between cells, and to the screen edge */
#define EX_TITLE_PT     20      /* reserved under each cell for the title */
/* THE HOT CORNER, in points from the top-right. Reachable without aiming --
 * the pointer clamps at (W-1, 0), so shoving it up and right always lands
 * inside -- and 18pt is small enough that crossing the corner on the way to
 * somewhere else does not sit in it. */
#define EX_CORNER_PT    18
#define EX_DWELL_MS     150      /* parked before it fires */

/* ---- Expose ---------------------------------------------------------------
 *
 * `ex_on` is the INTENT (the mode is up, or is being entered); `ex_t0` is the
 * transition in flight. Both together, because leaving is a state where the
 * intent is already "off" while the pixels are still on their way home --
 * ex_state() is the question every other reader actually wants to ask. */
int ex_on;                             /* the picker is up (or arriving)      */
static uint64_t ex_t0;                 /* monotonic ms transition start; 0 = still */
/* Leaving is not complete until the click that selected a thumbnail comes
 * back UP.  A slow full-screen landing frame can outlive the emulated mouse's
 * button-up packet; dropping ex_state() on the timer alone then lets a queued
 * down-state motion start a titlebar drag in the ordinary desktop underneath.
 * Keep modal pointer ownership after the pixels have landed, without keeping
 * the animation or its damage loop running. */
static int ex_wait_release;
int ex_hov = -1;                       /* grid slot under the pointer, or -1  */
static int ex_n;                       /* windows in the grid                 */
int ex_wi[MAXWIN];                     /* wins[] index per grid slot          */
struct drect ex_cell[MAXWIN];          /* the settled thumbnail rect per slot */

int ex_state(void) { return ex_on || ex_t0 != 0; }

/* THE TRIGGER. Two of them, and the reasoning for each is the input machinery
 * that already exists rather than a preference.
 *
 * A HOT CORNER, because the WM already tracks the pointer continuously and
 * already runs a per-pass tick (the dock's launch bounce is driven from it), so
 * "has the pointer been parked in the top-right for 150 ms" costs one compare
 * per loop pass and no new machinery at all. Top-RIGHT specifically: the menu
 * bar's left end is the app menu region and its right end is the clock, which
 * is text -- nothing there is clickable, so a pointer resting in that corner is
 * not on its way to anything. The dwell is what stops a pointer merely CROSSING
 * the corner from firing it, and `armed` is what stops it re-firing while the
 * pointer stays parked after the mode is dismissed.
 *
 * AND Cmd+E, because wm_shortcut already owns a closed, documented list of Cmd
 * chords (W/Q/M/Tab/`) and E was free -- so this costs one case label and takes
 * no keystroke away from any app. A gesture that only exists as a hot corner is
 * one a keyboard cannot reach. */
uint64_t ex_corner_t0;                 /* monotonic ms when pointer parked */
static int ex_corner_armed = 1;        /* leave the corner to re-arm the trigger */

/* ---- Expose: the grid ----------------------------------------------------
 *
 * Rebuilt at the moment the gesture starts and then held FIXED for the life of
 * the mode. Not recomputed per frame, and deliberately not recomputed when the
 * user picks a window: the grid somebody is looking at is the grid the windows
 * have to fly home from, and a layout that re-flowed under the pointer at the
 * instant of the click would move the target out from under the click that
 * chose it. */
static void ex_layout(void)
{
    ex_n = 0;
    /* Back-to-front, so slot order matches stacking order: the window you last
     * used lands in the last cell, and that stays true from one invocation to
     * the next as long as you have not restacked anything. A picker whose
     * items move for no reason the user can see is a picker you have to read
     * every time instead of aiming at. */
    for (int i = 0; i < wm_norder && ex_n < MAXWIN; i++)
        if (wm_wins[wm_order[i]].used && !wm_wins[wm_order[i]].panel) ex_wi[ex_n++] = wm_order[i];
    if (!ex_n) return;

    /* rows = ceil(sqrt(N)). This machine's world is NAPPS ~ 11 and one window
     * per app, so N is single digits and the packing question that a real
     * Expose has to answer (longest-side-first bin packing, so a wide window
     * and a tall one do not both get a square cell) does not arise: at N <= 9
     * a uniform grid wastes cell area, never legibility. The aspect fit below
     * is what actually makes a 16:9 browser and a square clock both readable. */
    int cols = 1;
    while (cols * cols < ex_n) cols++;
    int rows = (ex_n + cols - 1) / cols;

    int g = S(EX_GUTTER_PT), th = S(EX_TITLE_PT);
    int dx0, dy0, dw, dh;
    dock_geom(&dx0, &dy0, &dw, &dh);
    (void)dx0; (void)dw;
    int ax0 = g, ay0 = MBH + g, ax1 = wm_W - g, ay1 = dy0 - g;
    if (ay1 - ay0 < S(120)) ay1 = wm_H - g;         /* a dock taller than the screen */

    int cw = (ax1 - ax0) / cols, ch = (ay1 - ay0) / rows;
    for (int k = 0; k < ex_n; k++) {
        struct win *w = &wm_wins[ex_wi[k]];
        int r = k / cols, c = k - r * cols;
        /* The LAST row is CENTRED. Seven windows in a 3x3 leave two holes, and
         * a hole on the right of the bottom row reads as a layout bug rather
         * than as a count -- the grid should look built for what is in it. */
        int inrow = (r == rows - 1) ? ex_n - r * cols : cols;
        int rowx = ax0 + (ax1 - ax0 - inrow * cw) / 2;
        int bx = rowx + c * cw, by = ay0 + r * ch;
        int fitw = cw - g, fith = ch - g - th;
        if (fitw < 1) fitw = 1;
        if (fith < 1) fith = 1;
        /* PRESERVE ASPECT, and never magnify past 1:1. A 200x120 utility window
         * blown up to fill a 400x300 cell is a blurred lie about its own size,
         * and the single thing a picker must get right is which window is
         * which -- size is half of how that is recognised. */
        int s = 256;
        if (w->w > 0 && fitw * 256 / w->w < s) s = fitw * 256 / w->w;
        if (w->h > 0 && fith * 256 / w->h < s) s = fith * 256 / w->h;
        if (s > 256) s = 256;
        if (s < 16) s = 16;
        int tw = w->w * s / 256, thh = w->h * s / 256;
        if (tw < 1) tw = 1;
        if (thh < 1) thh = 1;
        int cx = bx + cw / 2, cy = by + (ch - th) / 2;
        ex_cell[k].x0 = cx - tw / 2;
        ex_cell[k].y0 = cy - thh / 2;
        ex_cell[k].x1 = ex_cell[k].x0 + tw;
        ex_cell[k].y1 = ex_cell[k].y0 + thh;
    }
}

/* Grid slot of a wins[] index, or -1. Answers -1 whenever the mode is down, so
 * every caller can ask unconditionally. */
int ex_slot(int wi)
{
    if (!ex_state()) return -1;
    for (int k = 0; k < ex_n; k++) if (ex_wi[k] == wi) return k;
    return -1;
}

/* Eased progress of the Expose transition: 0 = windows at their own frames,
 * 256 = windows in their cells. Leaving runs the same curve backwards, so a
 * cancelled gesture retraces the path it came in on rather than taking a
 * second, different route home. */
int ex_prog(void)
{
    if (!ex_t0 || wm_reduce_motion) return ex_on ? 256 : 0;
    uint64_t e = anim_elapsed(ex_t0);
    if (e >= EX_DUR_MS) return ex_on ? 256 : 0;
    int eased = ease_out((int)(e * 256 / EX_DUR_MS));
    return ex_on ? eased : 256 - eased;
}

/* Damage one grid slot -- its thumbnail, its shadow and its title, via the same
 * win_box every other damage path uses. */
static void ex_dirty_slot(int k)
{
    if (k < 0 || k >= ex_n) return;
    struct drect r;
    win_box(&wm_wins[ex_wi[k]], &r);
    dirty_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

/* The grid slot under a point, or -1. Only while the grid is STILL: hit-testing
 * a moving target is how a click lands on the window that happened to be
 * passing through, and the mode is only pickable once it has settled anyway. */
static int ex_hover_at(int x, int y)
{
    if (!ex_on || ex_t0) return -1;
    for (int k = ex_n - 1; k >= 0; k--) {
        const struct drect *c = &ex_cell[k];
        if (x >= c->x0 && x < c->x1 && y >= c->y0 && y < c->y1) return k;
    }
    return -1;
}

void ex_enter(void)
{
    if (ex_state()) return;
    /* Do not transfer pointer ownership in the middle of another gesture.
     * Expose deliberately consumes every release while it owns the pointer;
     * entering during a titlebar drag, resize, or app capture would therefore
     * strand that old owner in its pressed state when the picker later closes.
     * Once the hand comes up, a later shortcut or hot-corner dwell may enter. */
    if (wm_pointer_buttons() || wm_pointer_owned()) return;
    /* A dock fly still running would be a second animation moving the same
     * rectangle; land it first rather than interleave two owners. */
    for (int i = 0; i < MAXWIN; i++) if (wm_wins[i].used) win_fly_settle(&wm_wins[i]);
    ex_layout();
    if (ex_n == 0) return;                 /* nothing to pick between */
    ex_on = 1;
    ex_t0 = anim_stamp();
    ex_wait_release = 0;
    ex_hov = -1;
    for (int k = 0; k < ex_n; k++) win_box(&wm_wins[ex_wi[k]], &wm_wins[ex_wi[k]].anim_prev);
    /* ONE full-screen frame, for the dim -- which lands everywhere at once and
     * is honestly whole-screen damage. Everything after this frame pays only
     * for the windows that move; see wm_anim_tick. */
    dirty_full(FULL_EXPOSE_ENTER);
    /* The grid itself, once: a harness can then assert that a mid-flight rect
     * lies strictly BETWEEN the window's own frame and the cell it is aimed at,
     * which is the actual claim "it animated" makes. Without the destination
     * printed, an intermediate rectangle is just an unexplained number. */
    kprintf("[wm] expose on n=%d\n", ex_n);
    for (int k = 0; k < ex_n; k++)
        kprintf("[wm] expose cell %d win %d rect %d %d %d %d\n", k, ex_wi[k],
                ex_cell[k].x0, ex_cell[k].y0,
                ex_cell[k].x1 - ex_cell[k].x0, ex_cell[k].y1 - ex_cell[k].y0);
}

/* Leave the picker. `pick` is a wins[] index to bring to the front, or -1 to
 * leave the stacking exactly as it was. */
void ex_leave(int pick)
{
    if (!ex_on) return;
    if (pick >= 0 && wm_wins[pick].used) {
        struct win *w = &wm_wins[pick];
        /* Picking a minimised window un-minimises it, and does so WITHOUT a
         * dock fly: it is already on screen, in a cell, in front of the user.
         * Flying it to the dock and back out again would animate a journey it
         * is not making. */
        w->minimized = 0;
        w->min_t0 = 0;
        raise_win(pick);
    }
    ex_on = 0;
    ex_t0 = anim_stamp();
    ex_wait_release = 0;
    ex_hov = -1;
    for (int k = 0; k < ex_n; k++) win_box(&wm_wins[ex_wi[k]], &wm_wins[ex_wi[k]].anim_prev);
    dirty_full(FULL_EXPOSE_LEAVE); /* the raise re-stacks every overlap on screen */
    kprintf("[wm] expose off pick=%d\n", pick);
}

/* The picker's share of the pointer while it owns it (wm.c's
 * wm_expose_capture does the rest). Motion: the hovered thumbnail. */
int wm_expose_pointer(int x, int y)
{
    int h = ex_hover_at(x, y);
    if (h != ex_hov) {                 /* same idiom as the dock's hover: */
        ex_dirty_slot(ex_hov);         /* damage the one being left...    */
        ex_hov = h;
        ex_dirty_slot(h);              /* ...and the one being entered    */
    }
    return 1;
}

/* A press: a window brings it to the front and leaves. Anywhere else -- the
 * dimmed wallpaper, the dock, the menu bar -- leaves with the stacking exactly
 * as it was. Dismissing is the safe answer for every pixel that is not a
 * thumbnail, because the mode covers the whole screen and there is no
 * "somewhere else" for a click to usefully mean anything else. */
int wm_expose_click(int x, int y, int btn)
{
    (void)btn;
    int h = ex_hover_at(x, y);
    ex_leave(h >= 0 ? ex_wi[h] : -1);
    return 1;
}

/* EXPOSE's part of the animation pass.
 *
 * A window closing while the picker is up leaves a hole in the grid, which
 * is harmless. ALL of them closing leaves a dimmed desktop with nothing to
 * pick, the pointer still captured by wm_expose_capture and the keyboard
 * still swallowed -- recoverable by clicking, but a state the machine
 * should not sit in waiting to be rescued.
 *
 * 3. EXPOSE. The dim is constant while the mode is up (see render_region),
 *    so every frame between the two full ones pays for the windows that
 *    moved and for nothing else. */
void wm_expose_anim_tick(uint64_t t)
{
    if (ex_on && !ex_t0) {
        int alive = 0;
        for (int k = 0; k < ex_n; k++) if (wm_wins[ex_wi[k]].used) { alive = 1; break; }
        if (!alive) ex_leave(-1);
    }

    if (ex_t0) {
        if (wm_reduce_motion || anim_elapsed_at(ex_t0, t) >= EX_DUR_MS) {
            if (!ex_on && wm_pointer_buttons()) {
                /* The windows have reached home, but the selecting press still
                 * belongs to Expose. Draw that exact terminal frame once and
                 * retain ex_t0 solely as the modal-capture bit. The first
                 * all-up packet is consumed by wm_expose_capture; the next tick
                 * clears the mode below. */
                if (!ex_wait_release) {
                    ex_wait_release = 1;
                    dirty_full(FULL_EXPOSE_TERMINAL);
                }
            } else {
                ex_t0 = 0;
                ex_wait_release = 0;
                dirty_full(FULL_EXPOSE_TERMINAL); /* entering: the swept area. leaving: the dim. */
            }
        } else {
            for (int k = 0; k < ex_n; k++) {
                struct win *w = &wm_wins[ex_wi[k]];
                if (!w->used) continue;
                anim_damage(w);
                anim_trace(ex_on ? "expose" : "unexpose", ex_wi[k], w, ex_prog());
            }
        }
    }
}

/* Has the pointer been parked in the hot corner long enough? See the comment on
 * ex_corner_t0 for why this corner and why a dwell. */
static void wm_hotcorner_tick(void)
{
    if (!wm_desktop_prefs.hot_corner || g_locked || ex_state()) { ex_corner_t0 = 0; return; }
    int inside = (wm_pointer_x() >= wm_W - S(EX_CORNER_PT)) && (wm_pointer_y() < S(EX_CORNER_PT));
    if (!inside) { ex_corner_t0 = 0; ex_corner_armed = 1; return; }
    if (!ex_corner_armed) return;              /* fired already; leave to re-arm */
    if (!ex_corner_t0) {
        ex_corner_t0 = anim_stamp();
        return;
    }
    if (anim_elapsed(ex_corner_t0) < EX_DWELL_MS) return;
    ex_corner_armed = 0;
    ex_corner_t0 = 0;
    ex_enter();
}

/* Expose's per-pass work: the hot corner's dwell (wm_hotcorner_tick). Its
 * damage (entering Expose is a full frame) asks for its frames. */
int wm_expose_tick(uint64_t now)
{
    (void)now;
    wm_hotcorner_tick();
    return 0;
}
