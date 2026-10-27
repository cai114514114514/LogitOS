/* wm_panel.c -- panels.
 *
 * Panels (WINS_SET_PANEL, Spotlight): shown and hidden without a dock fly,
 * and their share of the pointer and of the main loop (none yet).
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
