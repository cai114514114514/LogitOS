#ifndef BROWSER_PAINT_H
#define BROWSER_PAINT_H

struct node;

/* Paint the current layout display list into the window viewport (vx,vy,vw,vh)
 * at the given pixel scroll, using the GUI render syscalls. */
void browser_paint(int vx, int vy, int vw, int vh, int scroll);

/* Hit-test a viewport-local point; on a link, copy its href (NUL-terminated)
 * into buf (<= max) and return 1, else 0. */
int  browser_hittest(int x, int y, int scroll, char *buf, int max);

/* The same hit test, but resolving to the DOM. Finds the topmost painted box
 * containing the point and reports BOTH what an event needs (`*node`, the
 * element the box came from -- a text box resolves to its parent element) and
 * what the default action needs (`href`, the link target, "" if none).
 *
 * The two are separate on purpose: a click inside <a><span>x</span></a> targets
 * the <span> for dispatch but navigates the <a>'s href, and a click on a plain
 * <button> has a target and no href at all. Returns 1 if any box was hit. */
int  browser_hittest_node(int x, int y, int scroll, struct node **node,
                          char *href, int max);

#endif /* BROWSER_PAINT_H */
