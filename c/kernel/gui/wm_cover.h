#ifndef LOGIT_WM_COVER_H
#define LOGIT_WM_COVER_H
/* THE COVER PREDICATE of the opaque-content shortcut (wm.c, opaque_content_start),
 * pure so a host test can drive it.
 *
 * An app canvas is copied with an opaque blit. When it covers a whole damage
 * rectangle R, the wallpaper and every lower window are overwritten, so the
 * compositor starts at that window and skips the layers beneath. That proof
 * only holds for pixels the canvas really owns. A window is a ROUNDED shape: the
 * pixels in its corner squares (radius x radius at each rounded corner) are
 * clipped back to whatever is beneath the window, so a rectangle that touches
 * one of them is NOT covered and the layers below must be drawn.
 *
 * (x, y, w, h) is the window frame, DEVICE pixels; the canvas owns the rows
 * from y + tbh down. corner_r is the window corner radius in device pixels
 * (clamped to min(w, h) / 2 like every rounded rect in the engine); the two
 * bottom corners are always rounded, the two top corners only with
 * WM_COVER_TOP_ROUND (a panel, or a window whose title bar is hidden: its
 * canvas then owns the top edge). Returns 1 when R lies inside the canvas rect
 * and touches no rounded corner square. */
#include "wm_draw2.h"    /* struct drect */

#define WM_COVER_TOP_ROUND 1u

static inline int wm_cover_ok(const struct drect *R, int x, int y, int w, int h,
                              int tbh, int corner_r, unsigned flags)
{
    if (R->x0 >= R->x1 || R->y0 >= R->y1) return 0;
    if (!(R->x0 >= x && R->x1 <= x + w && R->y0 >= y + tbh && R->y1 <= y + h))
        return 0;
#ifndef WM_COVER_NEGCTL_IGNORE_CORNERS
    int r = corner_r;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > 0) {
        int left  = R->x0 < x + r,  right = R->x1 > x + w - r;
        int bot   = R->y1 > y + h - r;
        int top   = (flags & WM_COVER_TOP_ROUND) && R->y0 < y + r;
        if ((left || right) && (bot || top)) return 0;
    }
#else
    (void)corner_r; (void)flags;
#endif
    return 1;
}
#endif
