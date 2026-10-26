#ifndef GFX_SHAPE_H
#define GFX_SHAPE_H

/* gfx_shape.h -- arcs and the shapes built from them (c/lib/gfx/geometry/gfx_arc.c).
 *
 * Every builder is ordinary path construction through gfx_move_to / gfx_line_to /
 * gfx_cubic_to, in the same USER coordinates as gfx_path_circle, so the current
 * path matrix and tolerance apply. Curves are chains of cubic pieces of at most
 * 90 degrees. Angles are 24.8 degrees; positive sweeps run clockwise on a
 * y-down screen (0 degrees is the +x direction, 90 degrees is +y). All integer. */

#include "gfx_path.h"

/* Elliptical arc around (cx,cy) from `start` through `sweep` (|sweep| capped at
 * 360 degrees). connect == 0 starts a new subpath (gfx_move_to) at the start
 * point, non-zero draws a line from the current point to it. */
void gfx_arc(struct gfx_path *p, int cx, int cy, int rx, int ry,
             int start_deg24_8, int sweep_deg24_8, int connect);

/* SVG "A" command from the current point to (x,y); `large` and `sweep` are the
 * large-arc and sweep flags. W3C SVG 1.1 F.6.5, including F.6.6 radius
 * scale-up when rx/ry are too small to span the endpoints (a zero radius, or
 * coincident endpoints, degrade to a line / nothing as F.6.2 says). The arc
 * ends exactly on (x,y). */
void gfx_arc_to_svg(struct gfx_path *p, int rx, int ry, int xrot_deg24_8,
                    int large, int sweep, int x, int y);

/* Closed ring segment between r_inner and r_outer (r_inner <= 0 is a pie
 * slice). |sweep| >= 360 degrees gives a full ring as two subpaths of opposite
 * winding. */
void gfx_path_sector(struct gfx_path *p, int cx, int cy, int r_outer, int r_inner,
                     int start, int sweep);

/* Rounded rectangle with elliptical corners; order tl, tr, br, bl. */
void gfx_path_rrect_ell(struct gfx_path *p, int x, int y, int w, int h,
                        const int rx[4], const int ry[4]);

/* Rounded rectangle with Figma-style corner smoothing. smoothing_q8 0 is the
 * circular corner and produces exactly gfx_path_rrect4(r,r,r,r); 256 is fully
 * continuous curvature. The corner extent along an edge, (1+s)*r, never exceeds
 * min(w,h)/2: smoothing is reduced first, then the radius clamped. */
void gfx_path_squircle(struct gfx_path *p, int x, int y, int w, int h, int r,
                       int smoothing_q8);

/* Angle of (x,y) in 24.8 degrees, (-180, 180], y down; max error 0.002 degrees. */
int gfx_atan2(int y, int x);

#endif /* GFX_SHAPE_H */
