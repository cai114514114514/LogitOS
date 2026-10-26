#ifndef GFX_PATH_H
#define GFX_PATH_H

/* gfx_path.h -- struct gfx_path, its builders and its device bounds. */

#include "gfx_math.h"

/* ------------------------------------------------------------------ path --
 * A path is built in USER coordinates and stored FLATTENED IN DEVICE SPACE:
 * the current matrix is applied as points are recorded, and curves are
 * subdivided against a device-space tolerance. That ordering is deliberate --
 * flattening in user space and transforming afterwards makes the segment count
 * a function of the wrong scale, so a shape enlarged 4x becomes visibly
 * polygonal. The cost is that re-transforming means rebuilding, which for an
 * immediate-mode UI is what happens anyway.
 *
 * Storage is the caller's. Nothing here allocates:
 *
 *     static int   pts[256 * 2];
 *     static int   subs[16];
 *     struct gfx_path p;
 *     gfx_path_init(&p, pts, 256, subs, 16);
 */

struct gfx_path {
    int *pt;             /* x,y interleaved, 24.8 DEVICE                     */
    int  npt, ptcap;
    int *sub;            /* point index where each subpath starts; bit 30
                          * (GFX_SUB_CLOSED) = gfx_close() ended it. ALWAYS
                          * read through gfx_path_sub_start()                */
    int  nsub, subcap;
    int  overflow;       /* storage ran out, OR gfx_path_matrix() was called
                          * mid-path (see its declaration below) -- either way
                          * a fill is then refused outright rather than drawn
                          * from a path that cannot be trusted                */
    struct gfx_matrix m; /* CTM applied as points are recorded               */
    int  cx, cy;         /* current point, device 24.8                       */
    int  sx, sy;         /* start of the open subpath                        */
    int  open;
    int  tol;            /* flattening tolerance, 24.8 device px             */
};

/* A subpath that gfx_close() ended carries GFX_SUB_CLOSED in its sub[] entry.
 * gfx_close() still emits no point (a fill closes every subpath anyway), so
 * the bit is the only record of the pen having been lifted by a close. Fills
 * ignore it; the stroker uses it to join the seam instead of capping it.
 * Every reader of sub[] must mask it: use gfx_path_sub_start(). */
#define GFX_SUB_CLOSED 0x40000000
#define GFX_SUB_START(v) ((v) & ~GFX_SUB_CLOSED)

static inline int gfx_path_sub_start(const struct gfx_path *p, int s)
{
    return GFX_SUB_START(p->sub[s]);
}
static inline int gfx_path_sub_closed(const struct gfx_path *p, int s)
{
    return (p->sub[s] & GFX_SUB_CLOSED) != 0;
}

void gfx_path_init(struct gfx_path *p, int *ptbuf, int ptcap, int *subbuf, int subcap);
void gfx_path_reset(struct gfx_path *p);          /* keeps the matrix + tolerance */
/* Sets the CTM applied to every point recorded FROM HERE ON (path coordinates
 * are transformed and flattened as they are built -- see the path comment
 * above). Calling this after the path already holds a point is refused: the
 * points already recorded were flattened under the OLD matrix, so a path with
 * two calls to this mid-build would silently carry two different transforms in
 * two halves of the same shape -- a plausible-looking wrong picture, and per
 * this file's rule (gfx_path.c's file comment) that is refused, not drawn. The
 * refusal reuses `overflow`: to a caller of gfx_fill/gfx_fill_mask "ran out of
 * point storage" and "recorded under two different transforms" are the same
 * fact -- this path cannot be trusted -- so they share one flag and raster()
 * (gfx_raster.c) needs exactly one check for both. Calling it before the first
 * point (fresh from gfx_path_init/gfx_path_reset, including more than once)
 * is the normal case and always allowed. */
void gfx_path_matrix(struct gfx_path *p, const struct gfx_matrix *m);
void gfx_path_tolerance(struct gfx_path *p, int tol256);

void gfx_move_to (struct gfx_path *p, int x, int y);
void gfx_line_to (struct gfx_path *p, int x, int y);
void gfx_quad_to (struct gfx_path *p, int cx, int cy, int x, int y);
void gfx_cubic_to(struct gfx_path *p, int c1x, int c1y, int c2x, int c2y, int x, int y);
void gfx_close   (struct gfx_path *p);

/* Shape sugar. Every one of these is ordinary path construction -- there is no
 * privileged rect or circle inside the rasterizer. */
void gfx_path_rect   (struct gfx_path *p, int x, int y, int w, int h);
void gfx_path_rrect  (struct gfx_path *p, int x, int y, int w, int h, int r);
void gfx_path_rrect4 (struct gfx_path *p, int x, int y, int w, int h,
                      int rtl, int rtr, int rbr, int rbl);
void gfx_path_ellipse(struct gfx_path *p, int cx, int cy, int rx, int ry);
void gfx_path_circle (struct gfx_path *p, int cx, int cy, int r);

/* Device-pixel bounding box (x0,y0 inclusive, x1,y1 exclusive). 0 if empty. */
int gfx_path_bounds(const struct gfx_path *p, int *x0, int *y0, int *x1, int *y1);

#endif /* GFX_PATH_H */
