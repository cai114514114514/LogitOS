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
#include "wm_bands.h"     /* struct wm_yrange (wm_backdrop_nocut) */

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

/* ---- BACKDROP REGIONS (wm_backdrop.c) -------------------------------------
 * A window that shows a backdrop region (SYS_GUI_BACKDROP) does NOT own the
 * pixels of that region: the compositor draws the material there from the
 * layers BENEATH the window, so they must be drawn. The cover proof therefore
 * also needs R to touch no region of the window. `reg` are the regions'
 * screen (back-buffer) rects, wm_backdrop_screen_rects. */
static inline int wm_cover_ok_regions(const struct drect *R, const struct drect *reg, int n)
{
    for (int i = 0; i < n; i++)
        if (R->x0 < reg[i].x1 && reg[i].x0 < R->x1 && R->y0 < reg[i].y1 && reg[i].y0 < R->y1)
            return 0;
    return 1;
}

/* An app colour as it may be stored in a window surface: the high byte is the
 * backdrop ink mask's (LOGIT_BACKDROP_MARK), so a raw app value never reaches
 * it. wm.c applies this to every legacy draw call that stores the app's colour
 * unchanged (see the audit in wm_backdrop.c). */
static inline unsigned wm_app_rgb(unsigned v) { return v & 0xFFFFFFu; }

/* Where a window's canvas lands in the back buffer: the sw x sh surface drawn
 * into the dw x dh rect at (ox, oy). 1:1 for a window at its own frame;
 * scaled for a fading thumbnail. */
struct wm_bd_place { int ox, oy, sw, sh, dw, dh; };

/* The compositor's side of the regions. The table is graphics_lock state; see
 * the header of wm_backdrop.c for who writes and who reads it.
 *   wm_backdrop_any          the slot has at least one region (the fast test)
 *   wm_backdrop_screen_rects regions' back-buffer rects (no halo)
 *   wm_backdrop_cover_ok     wm_cover_ok_regions over those
 *   wm_backdrop_extent       union of every region plus its sample halo
 *   wm_backdrop_dmg_grow     dmg_expand's step: grow r to contain each region
 *                            plus halo (clamped to the scr_w x scr_h screen)
 *                            that it touches; 1 if it grew
 *   wm_backdrop_nocut        append each region's sample rows that meet R to
 *                            the band splitter's no-cut list (wm_bands.h)
 *   wm_backdrop_composite_at the material of every region intersecting R,
 *                            over the back buffer, BEFORE the content blit
 *   wm_backdrop_blit_content the 1:1 content blit that skips MARK pixels
 *                            inside a live region, at surface origin (dx, dy)
 * `reduce` is Reduce Transparency: the material is solid and samples nothing,
 * so its halo is 0. */
int  wm_backdrop_any(int slot);
int  wm_backdrop_screen_rects(int slot, const struct wm_bd_place *pl, struct drect *out, int max);
int  wm_backdrop_cover_ok(const struct drect *R, int slot, const struct wm_bd_place *pl);
int  wm_backdrop_extent(int slot, const struct wm_bd_place *pl, int reduce, struct drect *out);
int  wm_backdrop_dmg_grow(struct drect *r, int slot, const struct wm_bd_place *pl,
                          int reduce, int scr_w, int scr_h);
int  wm_backdrop_nocut(int slot, const struct wm_bd_place *pl, int reduce,
                       const struct drect *R, struct wm_yrange *nc, int n, int max);
void wm_backdrop_composite_at(const struct drect *R, int win_slot,
                              const struct wm_bd_place *pl, struct ol_display *d);
void wm_backdrop_blit_content(const struct drect *R, int win_slot, int dx, int dy,
                              struct ol_display *d, const struct ol_pixel_target *src);
#endif
