/* wm_panel.h -- panels: what the other window-manager files use of wm_panel.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_PANEL_H
#define LOGIT_WM_PANEL_H

#include "wm_internal.h"

/* Show or hide a panel (no dock fly); input and per-pass hooks. */
void panel_show(struct win *w, int show);
/* Place a panel by the WINS_SET_PANEL rule (centred, top at H/4 - 40 pt). */
void panel_place(struct win *w);
/* WINS_SET_SIZE: new content size in points; 0, or -1 (not a panel). */
int panel_set_size(struct win *w, int cw_pt, int ch_pt);
int wm_panel_pointer(int x, int y);
int wm_panel_tick(uint64_t now);

#endif /* LOGIT_WM_PANEL_H */
