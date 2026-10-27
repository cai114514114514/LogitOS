/* wm_expose.h -- Exposé: what the other window-manager files use of wm_expose.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_EXPOSE_H
#define LOGIT_WM_EXPOSE_H

#include "wm_internal.h"

/* How dark the desktop goes behind the grid. The windows have to read as THE
 * CONTENT and the wallpaper as backdrop; at this depth a photograph is still
 * legible underneath, which is what says "your desktop is still there" rather
 * than "you are in a different application". */
#define EX_DIM_ALPHA    120

/* State: the picker's intent, the hovered slot, the grid. */
extern int ex_on;
extern int ex_hov;
extern int ex_wi[MAXWIN];
extern struct drect ex_cell[MAXWIN];
extern uint64_t ex_corner_t0;
int ex_state(void);
int ex_slot(int wi);
int ex_prog(void);

/* Entering and leaving (pick: a wm_wins index to raise, or -1). */
void ex_enter(void);
void ex_leave(int pick);

/* Its share of the pointer while it owns it, its animation part, the hot corner tick. */
int wm_expose_pointer(int x, int y);
int wm_expose_click(int x, int y, int btn);
void wm_expose_anim_tick(uint64_t t);
int wm_expose_tick(uint64_t now);

#endif /* LOGIT_WM_EXPOSE_H */
