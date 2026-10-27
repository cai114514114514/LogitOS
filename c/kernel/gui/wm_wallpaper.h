/* wm_wallpaper.h -- the desktop picture: what the other window-manager files use of wm_wallpaper.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_WALLPAPER_H
#define LOGIT_WM_WALLPAPER_H

#include "wm_internal.h"

/* The generation of the baked wallpaper; bumped on every bake and theme flip. */
extern unsigned g_wallpaper_gen;

/* The picture a theme shows (both themes: ui.wallpaper today), and the bake into `bg`. */
const char *wm_wallpaper_path_for_theme(int dark);
void draw_background(void);

/* Input and per-pass hooks. */
int wm_desktop_context(int x, int y);
int wm_wallpaper_tick(uint64_t now);

#endif /* LOGIT_WM_WALLPAPER_H */
