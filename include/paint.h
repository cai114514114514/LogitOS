#ifndef AQUA_PAINT_H
#define AQUA_PAINT_H

/* Paint the current layout display list into a viewport rectangle of the active
 * draw target (set via fb_target). Doc coordinate `it->y` maps to screen
 * `vy + it->y - scroll`; clipped to [vy, vy+vh). */
void paint_viewport(int vx, int vy, int vw, int vh, int scroll);

/* Hit-test a viewport-local point (x,y relative to the viewport origin) at the
 * given scroll; if it lands on an item carrying an href, copy it (NUL-term) into
 * `buf` (<= max) and return 1, else 0. */
int  paint_hittest(int x, int y, int scroll, char *buf, int max);

#endif /* AQUA_PAINT_H */
