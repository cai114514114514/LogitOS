/* wm_reveal.h -- the theme reveal: what the other window-manager files use of
 * wm_reveal.c. Include wm_internal.h (which includes this), not this file on
 * its own. */
#ifndef LOGIT_WM_REVEAL_H
#define LOGIT_WM_REVEAL_H

#include "wm_internal.h"

/* A theme flip is about to happen, asked for at screen point (cx, cy) in
 * device px, or (-1, -1) when it came from no point (SYS_UI_DARK). Called by
 * wm_set_dark BEFORE g_ui_dark changes. 0: there is no reveal, do the
 * instant swap as always. Non-zero: the reveal owns the swap from here. */
int  wm_reveal_begin(int cx, int cy);
/* Non-zero while a reveal owns the front buffer: render_present then leaves
 * presenting to it, and dirty_full tells it the whole screen changed. */
int  wm_reveal_active(void);
/* Window w flushed a new frame (NULL: the whole screen changed). A reveal
 * patches a late repaint into its picture. */
void wm_reveal_note_flush(const struct win *w);
/* After the frame's presents: the reveal shows its next annulus. */
void wm_reveal_present(void);
/* Per-pass work; see wm_run's tick contract. */
int  wm_reveal_tick(uint64_t now);

/* wm.c: the whole screen as the next full frame would compose it, written
 * into dst (W x H, stride == W) instead of `back` and not presented -- the
 * reveal's off-screen `new` picture. 0, or -1 when refused. */
int  wm_render_full_into(uint32_t *dst, int stride);

#endif /* LOGIT_WM_REVEAL_H */
