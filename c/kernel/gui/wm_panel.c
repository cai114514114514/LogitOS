/* wm_panel.c -- panels.
 *
 * Panels (WINS_SET_PANEL, Spotlight): placed, sized (WINS_SET_SIZE), shown
 * and hidden without a dock fly, and their share of the pointer and of the
 * main loop (none yet).
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

/* A panel is shown and hidden, not minimised: the same `minimized` state every
 * hit test, focus rule and compositor path already honours, but set without
 * the flight to the dock -- a panel has no dock icon to fly to. */
void panel_show(struct win *w, int show)
{
    if (!w->panel || w->minimized == !show) return;
    dirty_win(w);
    w->minimized = !show;
    w->min_t0 = 0;
    if (show) raise_win((int)(w - wm_wins));
    dirty_full(FULL_ZORDER_SYSTEM);
    kprintf("[wm] panel %s win=%d\n", show ? "show" : "hide", (int)(w - wm_wins));
}

/* Hover for panels (Spotlight): nothing reacts to it yet. */
int wm_panel_pointer(int x, int y) { (void)x; (void)y; return 0; }

/* Per-pass idle work for panels: none. */
int wm_panel_tick(uint64_t now) { (void)now; return 0; }

/* Where a panel sits: horizontally centred, its top a quarter of the way down
 * less 40 pt, never closer than 8 pt to the menu bar. WINS_SET_PANEL places
 * a panel with this and WINS_SET_SIZE re-places it, so a panel that grows or
 * shrinks keeps its top edge and its centre line (design 5.6). */
void panel_place(struct win *w)
{
    w->x = (wm_W - w->w) / 2;
    w->y = wm_H / 4 - S(40);
    if (w->y < MBH + S(8)) w->y = MBH + S(8);
}

/* A panel's whole footprint for damage: the window box (frame, the focused
 * window shadow it is drawn with today, backdrop sample halos -- dirty_win),
 * and the overlay shadow's reach, the deepest a panel takes in the design's
 * ladder (E6, 3.3), so the damage stays honest when the chrome moves panels
 * onto it. The stale-rectangle bug (P11a) was exactly a hide or a resize that
 * damaged less than the shadow had painted. */
static void panel_damage(const struct win *w)
{
    dirty_win(w);
    int e = wm_sh_ext(WMSH_OVERLAY);
    int x, y, ww, wh, a;
    if (e > WSH_EXT(1) && win_draw_rect(w, &x, &y, &ww, &wh, &a))
        dirty_rect(x - e, y - e, ww + 2 * e, wh + 2 * e);
}

/* WINS_SET_SIZE: a panel's new CONTENT size in points (a panel has no
 * titlebar, so content is the frame). Damages the old footprint, re-places,
 * reallocates the canvas NOW -- in the syscall, under the graphics and state
 * locks the resize pass also holds -- so the EV_RESIZE the app reads describes
 * a canvas it can already draw at, and damages the new footprint. -1 for a
 * non-panel or a zero size; a size past the screen is clamped. */
int panel_set_size(struct win *w, int cw_pt, int ch_pt)
{
    if (!w->panel || cw_pt < 1 || ch_pt < 1) return -1;
    int nw = S(cw_pt), nh = S(ch_pt);
    int top = wm_H / 4 - S(40);
    if (top < MBH + S(8)) top = MBH + S(8);
    if (nw > wm_W) nw = wm_W;
    if (nh > wm_H - top) nh = wm_H - top;
    if (nw < 1 || nh < 1) return -1;
    if (nw == w->w && nh == w->h) return 0;
    panel_damage(w);                     /* where it was: shadow included */
    w->w = nw;
    w->h = nh;
    panel_place(w);
    win_apply_size(w);                   /* canvas, EV_RESIZE, dirty_win */
    panel_damage(w);                     /* where it is now */
    kprintf("[wm] panel size win=%d %d %d %d %d\n", (int)(w - wm_wins), w->x, w->y, w->w, w->h);
    return 0;
}
