/* wm_place.h -- window placement: what the other window-manager files use of wm_place.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_PLACE_H
#define LOGIT_WM_PLACE_H

#include "wm_internal.h"

/* ===========================================================================
 * SNAPPING ("Fluid" P3): drag a window to the left or right edge of the
 * screen and it takes that half of the desktop; drag it into the menu bar and
 * it fills the desktop (the zoom). A translucent preview shows where it will
 * land while the pointer is in a zone; letting go applies it. Dragging a
 * snapped window away gives it back the size it had, with the pointer at the
 * same place along its titlebar -- the half-screen size is where it was put,
 * not what it is.
 *
 * The halves are cut from zoom_rect, so they stop above the dock for the same
 * reason the zoom does. The preview is deliberately NOT glass: its damage is
 * its own box and nothing it covers has to be re-frosted.
 * ======================================================================== */
enum { SNAP_NONE, SNAP_LEFT, SNAP_RIGHT, SNAP_TOP };

/* Placement: the cascade slot, the work-area fit, the size floor, every frame change. */
void wm_place_cascade(struct win *w);
void win_fit_work_area(struct win *w, int resize);
void win_min_frame(const struct win *w, int *mw, int *mh);
void win_set_frame(struct win *w, int x, int y, int ww, int wh);

/* Zoom. */
void win_set_zoom(struct win *w, int on);
void win_break_zoom(struct win *w);

/* Snapping (g_snap_zone is a SNAP_* zone) and its preview. */
extern int g_snap_zone;
int snap_zone_at(int px, int py);
void snap_set(int zone);
void snap_box(struct drect *r);
void draw_snap_preview(void);
void win_snap(struct win *w, int zone);

/* The resize grab under a point. */
int resize_hit(int x, int y, int *edge);

#endif /* LOGIT_WM_PLACE_H */
