/* wm_dock.h -- the dock and the app registry: what the other window-manager files use of wm_dock.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_DOCK_H
#define LOGIT_WM_DOCK_H

#include "wm_internal.h"

/* Geometry: every dock box derives from dock_geom. */
void dock_geom(int *x0, int *y0, int *dw, int *dh);
void dock_icon_box(int slot, struct drect *r);
void dock_box(struct drect *r);
int wm_dock_glass_rect(struct wm_glass_region *out, int max);

/* Painting and damage. */
void draw_dock(void);
void dock_tile(int i, int cx, int cy, int sz);
void dirty_dock(void);

/* Input: a press on an icon, the hover crossing an icon boundary. */
int wm_dock_click(int x, int y, int btn);
int wm_dock_pointer(int x, int y, int ox, int oy);

/* The app registry the dock shows. */
void scan_apps(void);
int dock_app_count(void);
int dock_slot_for(const struct win *w);
const char *dock_file_for_ext(const char *ext);
void dock_launch_bounce(const char *aex_file);

/* Per-pass hook: the autohide slide. */
int wm_dock_tick(uint64_t now);

#endif /* LOGIT_WM_DOCK_H */
