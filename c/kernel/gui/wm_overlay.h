/* wm_overlay.h -- the About / power panel and the Cmd+Tab switcher: what the other window-manager files use of wm_overlay.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_OVERLAY_H
#define LOGIT_WM_OVERLAY_H

#include "wm_internal.h"

/* ---- the "About" / "Shut Down" / "Restart" modal panel -------------------
 * A second, independent overlay rather than a fourth menu-item kind, because
 * it needs its own input capture the way Expose does: once it is up, a click
 * is either a button or a dismissal, never a window or dock click that leaks
 * through. Mutually exclusive with an open menu -- every path that opens one
 * closes the other first. */
enum { OV_NONE, OV_ABOUT, OV_CONFIRM_SHUTDOWN, OV_CONFIRM_RESTART };
struct wm_switcher { int open, shown, n, sel; int list[MAXWIN]; uint64_t t0; };

/* The About / Shut Down / Restart panel (g_overlay is an OV_* kind). */
extern int g_overlay;
void overlay_box(struct drect *r);
void overlay_open(int kind);
void overlay_close(void);
void draw_overlay_panel(void);
int wm_overlay_pointer(int x, int y);
int wm_overlay_click(int x, int y, int btn);

/* The Cmd+Tab switcher and the most-recently-used order it reads. */
extern struct wm_switcher g_sw;
extern int g_mru[MAXWIN], g_nmru;
void mru_touch(int wi);
void sw_box(struct drect *r);
void draw_switcher(void);
void sw_open(int dir);
void sw_step(int dir);
void sw_commit(void);
void sw_cancel(void);

/* Both panels' glass; the per-pass hook (the switcher's reveal and Cmd watch). */
int wm_overlay_glass_rect(struct wm_glass_region *out, int max);
int wm_overlay_tick(uint64_t now);

#endif /* LOGIT_WM_OVERLAY_H */
