#ifndef BROWSER_PAINT_H
#define BROWSER_PAINT_H

/* Paint the current layout display list into the window viewport (vx,vy,vw,vh)
 * at the given pixel scroll, using the GUI render syscalls. */
void browser_paint(int vx, int vy, int vw, int vh, int scroll);

/* Hit-test a viewport-local point; on a link, copy its href (NUL-terminated)
 * into buf (<= max) and return 1, else 0. */
int  browser_hittest(int x, int y, int scroll, char *buf, int max);

#endif /* BROWSER_PAINT_H */
