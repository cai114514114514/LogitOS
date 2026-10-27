/* wm_place.c -- window placement.
 *
 * Where windows go: the cascade a new window opens at, the work-area fit,
 * the WM's size floor, every frame change (win_set_frame), zoom, the
 * left/right/top snaps with their preview, and the resize grab's hit test.
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

static int cascade;

/* INITIAL PLACEMENT: a new window opens at the next of six cascade slots,
 * 28 points apart. win_fit_work_area() then pulls it into the work area. */
void wm_place_cascade(struct win *w)
{
    w->x = S(110 + cascade * 28); w->y = S(70 + cascade * 28);
    cascade = (cascade + 1) % 6;
}

/* ---------- resize, zoom, minimise ----------
 *
 * Geometry lives here, in device pixels, and nowhere else. Every gesture --
 * an edge drag, the green light, a double-click, Cmd+M -- funnels into
 * win_set_frame(), so there is exactly one place that clamps, exactly one that
 * reports damage, and no way to add a fifth gesture that forgets either. */

/* The smallest OUTER frame this window may have: the WM's floor, raised by
 * whatever the app asked for, plus the titlebar the app does not own. */
void win_min_frame(const struct win *w, int *mw, int *mh)
{
    int cw = w->min_w_pt > MIN_CONTENT_W_PT ? w->min_w_pt : MIN_CONTENT_W_PT;
    int ch = w->min_h_pt > MIN_CONTENT_H_PT ? w->min_h_pt : MIN_CONTENT_H_PT;
    *mw = S(cw);
    *mh = TBH + S(ch);
}

/* Move and/or resize a window, damaging BOTH the region it left and the region
 * it now occupies.
 *
 * Reporting only the destination is the whole failure mode of a partial
 * compositor, and a resize is where it is easiest to commit: when a window
 * SHRINKS the two boxes are nested, the new one is entirely inside the old,
 * and every counter-based check still passes while a band of the previous
 * frame stays on screen untouched. WM_RESIZE_DAMAGE_LIE above makes exactly
 * that mistake on purpose. */
void win_set_frame(struct win *w, int x, int y, int ww, int wh)
{
    int mw, mh;
    win_min_frame(w, &mw, &mh);
    if (ww < mw) ww = mw;
    if (wh < mh) wh = mh;
    /* The canvas is allocated at the frame's size, so a frame larger than the
     * display would allocate more than the screen -- and w*h*4 is the one
     * multiplication in this file that a user's hand controls directly. */
    if (ww > wm_W) ww = wm_W;
    if (wh > wm_H) wh = wm_H;
    if (y < MBH) y = MBH;                     /* never under the menu bar */
    if (w->x == x && w->y == y && w->w == ww && w->h == wh) return;
    wm_resize_damage_old(w);                  /* where it was (wm.c: the control) */
    w->x = x; w->y = y; w->w = ww; w->h = wh;
    dirty_win(w);                             /* where it now is */
}

/* The rectangle a zoomed window fills.
 *
 * "Maximize" here means FILL THE DESKTOP, not full-screen, and the two are
 * different products. A full-screen mode hides the menu bar, and this desktop
 * has no reveal gesture to get it back -- the clock and the dark-mode switch
 * would simply be gone, with the keyboard offering no way to return. The dock
 * is the same argument from the other end: it is composited on top and windows
 * already slide under it, so a window that filled to the bottom edge would put
 * its last 70 points permanently behind glass. Stopping above the dock is what
 * macOS's zoom does with the Dock pinned, and it is the honest answer for a
 * dock that cannot auto-hide. */
static void zoom_rect(int *x, int *y, int *ww, int *wh)
{
    int dx0, dy0, dw, dh;
    dock_geom(&dx0, &dy0, &dw, &dh);
    (void)dx0; (void)dw; (void)dh;
    *x = 0;
    *y = MBH;
    *ww = wm_W;
    *wh = dy0 - S(8) - MBH;
    if (*wh < TBH + S(MIN_CONTENT_H_PT)) *wh = wm_H - MBH;   /* a dock taller than the screen */
}

/* Keep a window the WM PLACES inside the work area -- zoom_rect(): below the
 * menu bar, above the dock, inside the screen.
 *
 * Only for placement the WM does on its own (a new window, and a window an app
 * has just grown with SYS_GUI_WIN_MIN). A window a person drags is theirs to put
 * half under the dock or off the edge, as on macOS, so win_set_frame() and the
 * gestures do not call this.
 *
 * Why it exists (found by a desktop tour on the macOS dev host, 2026-10-26):
 * creation clamped against the whole screen, not the dock, and SYS_GUI_WIN_MIN
 * grew a window IN PLACE. TextEdit and Code Studio declare minimums wider than
 * the space right of their cascade slot, so they opened running off the right
 * edge; Settings, Terminal and the Browser opened with their bottom rows --
 * Settings' Apply button among them -- permanently behind the dock glass.
 *
 * With `resize` a frame larger than the area is shrunk to it (both callers pass
 * 1; 0 moves only, which is what creation first did -- it left TextEdit and Code
 * Studio taller than the area, see SYS_GUI_CREATE), except that
 * win_set_frame() never goes below the app's declared minimum; such
 * a window is pinned to the area's top-left corner and overflows right/down,
 * which is the least bad of the choices. Every placement is logged as
 * "[wm] place" with the area, so the gate reads the WM's own numbers.
 * -DWM_PLACE_NEGCTL_NO_FIT restores the old behaviour for the control. */
void win_fit_work_area(struct win *w, int resize)
{
    int ax, ay, aw, ah;
    zoom_rect(&ax, &ay, &aw, &ah);
#ifndef WM_PLACE_NEGCTL_NO_FIT
    if (!w->panel) {
        int x = w->x, y = w->y, ww = w->w, wh = w->h;
        if (resize && ww > aw) ww = aw;
        if (resize && wh > ah) wh = ah;
        /* A window that does not fit at its cascade slot is CENTRED in the
         * area, not pushed flush against the far edge: the first version did
         * the latter, and the 2026-10-26 release tour showed TextEdit, Code
         * Studio and the Browser all glued to the screen's right edge with a
         * 160-point gutter on the left. Centring is what macOS does with a
         * window too big for its cascade position. */
        if (x + ww > ax + aw) x = ax + (aw - ww) / 2;
        if (y + wh > ay + ah) y = ay + (ah - wh) / 2;
        if (x < ax) x = ax;
        if (y < ay) y = ay;
        if (resize) win_set_frame(w, x, y, ww, wh);
        else { w->x = x; w->y = y; }
    }
#endif
    kprintf("[wm] place win=%d frame %d %d %d %d work %d %d %d %d\n",
            (int)(w - wm_wins), w->x, w->y, w->w, w->h, ax, ay, aw, ah);
}

/* A ZOOM reaches the bottom of the screen: a zoomed frontmost window hides the
 * dock (see DOCK AUTOHIDE), so there is nothing to stop above. zoom_rect()
 * stays the work area for placement and for the halves, which leave the dock up. */
static void zoom_full_rect(int *x, int *y, int *ww, int *wh)
{
    *x = 0; *y = MBH; *ww = wm_W; *wh = wm_H - MBH;
}

void win_set_zoom(struct win *w, int on)
{
    on = on ? 1 : 0;
    if (on == w->zoomed) return;
    w->snapped = 0;
    if (on) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        int x, y, ww, wh;
        zoom_full_rect(&x, &y, &ww, &wh);
        w->zoomed = 1;
        win_set_frame(w, x, y, ww, wh);
    } else {
        w->zoomed = 0;
        /* EXACTLY the frame it had. Not "roughly where it was" and not
         * re-derived from the app's original gui_create() size -- a window the
         * user had already dragged and resized before zooming would land
         * somewhere it has never been, which reads as the machine losing the
         * window rather than restoring it. */
        win_set_frame(w, w->sx, w->sy, w->sw, w->sh);
    }
}

/* A manual drag or resize of a zoomed window un-zooms it and FORGETS the saved
 * frame. Keeping the frame would mean the green light later teleports the
 * window back to a position the user has since deliberately moved it away
 * from; a zoom the user has begun editing is no longer a zoom.
 *
 * CALLED ON MOVEMENT, NEVER ON THE PRESS. Calling it when the button goes down
 * broke double-click-to-restore in a way that looked like the double-click not
 * being detected at all: the FIRST click of the pair took the drag branch and
 * silently cleared `zoomed` while leaving the frame where it was, so the second
 * click's toggle read "not zoomed" and zoomed a window that was already filling
 * the screen. Nothing moved, twice. Touching a titlebar is not editing a zoom;
 * moving the window is. */
void win_break_zoom(struct win *w)
{
    if (!w->zoomed) return;
    w->zoomed = 0;
    w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
}
int g_snap_zone;

static void snap_frame(int zone, int *x, int *y, int *ww, int *wh)
{
    if (zone == SNAP_TOP) { zoom_full_rect(x, y, ww, wh); return; }   /* the preview IS the zoom */
    zoom_rect(x, y, ww, wh);
    if (zone == SNAP_LEFT || zone == SNAP_RIGHT) {
        int half = *ww / 2;
        if (zone == SNAP_RIGHT) *x += *ww - half;
        *ww = half;
    }
}

void snap_box(struct drect *r)
{
    if (!g_snap_zone) { r->x0 = r->y0 = r->x1 = r->y1 = 0; return; }
    int x, y, ww, wh;
    snap_frame(g_snap_zone, &x, &y, &ww, &wh);
    r->x0 = x + S(6); r->y0 = y + S(6); r->x1 = x + ww - S(6); r->y1 = y + wh - S(6);
}

void snap_set(int zone)
{
    if (zone == g_snap_zone) return;
    struct drect r;
    snap_box(&r); if (r.x1 > r.x0) dirty_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    g_snap_zone = zone;
    snap_box(&r); if (r.x1 > r.x0) dirty_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

int snap_zone_at(int px, int py)
{
    if (py < MBH) return SNAP_TOP;
    if (px <= S(1)) return SNAP_LEFT;
    if (px >= wm_W - 1 - S(1)) return SNAP_RIGHT;
    return SNAP_NONE;
}

void draw_snap_preview(void)
{
    struct drect r; snap_box(&r);
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (w <= 0 || h <= 0) return;
    fb_blend_round_rect(r.x0, r.y0, w, h, S(12), 94, 150, 255, 60);
    fb_blend_round_rect(r.x0, r.y0, w, S(3), S(2), 94, 150, 255, 140);
}

void win_snap(struct win *w, int zone)
{
    const char *name = zone == SNAP_LEFT ? "left" : zone == SNAP_RIGHT ? "right" : "top";
    if (zone == SNAP_TOP) {
        w->snapped = 0;
        win_set_zoom(w, 1);
    } else {
        /* Remember the size to give back; the position is wherever the drag
         * that snapped it started, and the drag that un-snaps it decides the
         * next one. */
        if (!w->snapped) { w->sw = w->w; w->sh = w->h; }
        w->zoomed = 0;
        w->snapped = zone;
        int x, y, ww, wh;
        snap_frame(zone, &x, &y, &ww, &wh);
        win_set_frame(w, x, y, ww, wh);
    }
    kprintf("[wm] snap win=%d zone=%s frame %d %d %d %d was %d %d\n", (int)(w - wm_wins), name,
            w->x, w->y, w->w, w->h, w->sw, w->sh);
}

/* Which edges of `w` the point (x,y) grabs, or 0 for "not a resize".
 *
 * A corner is a longer stretch of BOTH edges rather than the intersection of
 * two bands: the intersection is RESIZE_IN+RESIZE_OUT points square, which is
 * a target nobody hits on purpose. RESIZE_CORNER_PT along each edge is the
 * target people actually aim at. */
static int resize_edge_at(const struct win *w, int x, int y)
{
    if (w->minimized) return 0;
    int out = S(RESIZE_OUT_PT), in = S(RESIZE_IN_PT), corner = S(RESIZE_CORNER_PT);
    int x0 = w->x, y0 = w->y, x1 = w->x + w->w, y1 = w->y + w->h;
    if (x < x0 - out || x > x1 + out || y < y0 - out || y > y1 + out) return 0;
    int e = 0;
    if (x >= x0 - out && x <= x0 + in) e |= RZ_L;
    if (x <= x1 + out && x >= x1 - in) e |= RZ_R;
    if (y >= y0 - out && y <= y0 + in) e |= RZ_T;
    if (y <= y1 + out && y >= y1 - in) e |= RZ_B;
    if (!e) return 0;
    /* Extend whichever edges were hit along the perpendicular, so the last
     * `corner` points of a side resize both axes. */
    if (e & (RZ_L | RZ_R)) {
        if (y <= y0 + corner) e |= RZ_T;
        else if (y >= y1 - corner) e |= RZ_B;
    }
    if (e & (RZ_T | RZ_B)) {
        if (x <= x0 + corner) e |= RZ_L;
        else if (x >= x1 - corner) e |= RZ_R;
    }
    /* A window pinned under the menu bar cannot be pulled further up, and
     * offering the grab anyway gives a cursor that promises something the
     * clamp will refuse. */
    if ((e & RZ_T) && w->y <= MBH) e &= ~RZ_T;
    return e;
}

/* The topmost window offering a resize grab at (x,y), and the edges. The
 * front-to-back walk matters: a window's outer band overhangs whatever is
 * behind it, and the one you can see is the one that must win. */
int resize_hit(int x, int y, int *edge)
{
    for (int i = wm_norder - 1; i >= 0; i--) {
        struct win *w = &wm_wins[wm_order[i]];
        if (!w->used || w->minimized) continue;
        int e = resize_edge_at(w, x, y);
        if (e) { *edge = e; return wm_order[i]; }
        /* Stop at the first window that CONTAINS the point: a window behind it
         * may have an edge running under this one, and a grab that reaches
         * through an opaque window is a grab aimed at something invisible. */
        if (in_rect(x, y, w->x, w->y, w->w, w->h)) return -1;
    }
    return -1;
}
