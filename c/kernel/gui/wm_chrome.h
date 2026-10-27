/* wm_chrome.h -- window chrome: what the other window-manager files use of wm_chrome.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_CHROME_H
#define LOGIT_WM_CHROME_H

#include "wm_internal.h"

/* The frame and the titlebar/content seam; the strip an animated window's
 * titlebar is minified from. */
void draw_frame(struct win *w, int focused);
void draw_titlebar_sep(int x, int y, int ww);
struct surface *win_chrome_strip(struct win *w, int focused);

/* The rounded content: its radius, its cover flags, the corner group. */
int win_corner_r(const struct win *w);
unsigned win_cover_flags(const struct win *w);
int content_group_begin(struct ol_display_group *g, int x, int y, int cw, int ch, int r, unsigned flags);
void content_group_end(struct ol_display_group *g);

/* Each window's glass titlebar, for wm_glass_regions. */
int wm_chrome_glass_rects(struct wm_glass_region *out, int max);

/* Input (the lights and the double-click) and the per-pass hook. */
int wm_chrome_click(int wi, int x, int y, int btn);
int wm_chrome_pointer(int wi, int x, int y);
int wm_chrome_tick(uint64_t now);

#endif /* LOGIT_WM_CHROME_H */
