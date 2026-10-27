/* wm_glass.h -- the chrome's materials: what the other window-manager files use of wm_glass.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_GLASS_H
#define LOGIT_WM_GLASS_H

#include "wm_internal.h"

/* The frame's live-backdrop glass cost (wm.c resets and reports them). */
extern uint64_t wm_frame_glass_ns, wm_frame_glass_px;

/* Chrome glass by material kind; the app's legacy SYS_GUI_GLASS. */
void wm_glass_panel(enum wm_mat kind, int x, int y, int w, int h, int r_pt, unsigned cut, int alpha);
void wm_glass_app(int x, int y, int w, int h, int radius, uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta);

/* Every chrome glass panel on screen this frame, for dmg_expand and par_nocut. */
int wm_glass_regions(struct wm_glass_region *out, int max);

/* Per-pass hook. */
int wm_glass_tick(uint64_t now);

#endif /* LOGIT_WM_GLASS_H */
