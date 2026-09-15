/* Open Logit -- paths: building, transforming and flattening.
 *
 * A path is recorded in USER coordinates and stored FLATTENED IN DEVICE SPACE.
 * The current matrix is applied to every control point as it arrives and the
 * curve is then subdivided against a device-space tolerance. Doing it the other
 * way round -- flatten in user space, transform the polyline afterwards -- makes
 * the segment count a function of the wrong scale, so the same path enlarged 4x
 * turns visibly polygonal while a shrunken one wastes segments it cannot show.
 * De Casteljau subdivision is affine-invariant, so transforming first costs
 * nothing in accuracy.
 *
 * Nothing here allocates. Storage is the caller's two arrays; running out sets
 * `overflow`, and a fill of an overflowed path is REFUSED rather than drawn
 * truncated -- a half-recorded path closes across the middle of the shape, which
 * renders as a plausible-looking wrong picture, and a plausible wrong picture is
 * the one failure mode worth being loud about. */
#include "gfx.h"

#define MAX_DEPTH 10          /* subdivision recursion cap: 1024 segments/curve */

/* 4/3 * tan(pi/8) -- the constant that makes a cubic approximate a quarter
 * circle. Its worst radial error is 2.7e-4 of the radius, i.e. a fifth of a
 * 24.8 step at r = 256 px, so no arc in this system can show it. */
#define KAPPA 36195           /* 0.5522847498 in 16.16 */

void gfx_path_init(struct gfx_path *p, int *ptbuf, int ptcap, int *subbuf, int subcap)
{
    p->pt = ptbuf; p->ptcap = ptcap; p->npt = 0;
    p->sub = subbuf; p->subcap = subcap; p->nsub = 0;
    p->overflow = 0;
    p->cx = p->cy = p->sx = p->sy = 0;
    p->open = 0;
    p->tol = GFX_ONE / 32;                 /* 1/32 device pixel */
    gfx_m_identity(&p->m);
}

void gfx_path_reset(struct gfx_path *p)
{
    p->npt = 0; p->nsub = 0; p->overflow = 0; p->open = 0;
    p->cx = p->cy = p->sx = p->sy = 0;
}

/* See the declaration in gfx.h for why this refuses mid-path: recorded points
 * are already flattened under the OLD matrix, so replacing it now would bake
 * two different transforms into two halves of one path silently. Reusing
 * `overflow` rather than adding a second field+check is deliberate: raster()
 * only has to consult one flag, and to a caller of gfx_fill/gfx_fill_mask the
 * two causes mean the identical thing -- this path's geometry is not to be
 * trusted, refuse the fill outright. `npt > 0` is the right test, not `open`:
 * a matrix set between gfx_close() and the next gfx_move_to() (open == 0, but
 * npt > 0 from the finished subpath) is exactly as unsound as one set mid-
 * subpath, since the earlier points are still flattened under the old matrix. */
void gfx_path_matrix(struct gfx_path *p, const struct gfx_matrix *m)
{
    if (p->npt > 0) { p->overflow = 1; return; }
    p->m = *m;
}
void gfx_path_tolerance(struct gfx_path *p, int tol256) { p->tol = tol256 > 0 ? tol256 : 1; }

/* ---- raw device-space appenders (post-transform) ---- */

static void push_pt(struct gfx_path *p, int x, int y)
{
    if (p->npt >= p->ptcap) { p->overflow = 1; return; }
    p->pt[p->npt * 2] = x; p->pt[p->npt * 2 + 1] = y;
    p->npt++;
}

static void start_sub(struct gfx_path *p, int x, int y)
{
    if (p->nsub >= p->subcap) { p->overflow = 1; return; }
    p->sub[p->nsub++] = p->npt;
    push_pt(p, x, y);
    p->open = 1;
}

static void dev_line(struct gfx_path *p, int x, int y)
{
    if (!p->open) { start_sub(p, p->cx, p->cy); }
    if (p->npt > 0 && p->pt[(p->npt - 1) * 2] == x && p->pt[(p->npt - 1) * 2 + 1] == y)
        return;                                    /* exact repeat: no edge */
    push_pt(p, x, y);
}

/* ---- public builders ---- */

void gfx_move_to(struct gfx_path *p, int x, int y)
{
    int dx, dy;
    gfx_m_apply(&p->m, x, y, &dx, &dy);
    p->cx = p->sx = dx; p->cy = p->sy = dy;
    p->open = 0;                                   /* the subpath opens on first segment */
}

void gfx_line_to(struct gfx_path *p, int x, int y)
{
    int dx, dy;
    gfx_m_apply(&p->m, x, y, &dx, &dy);
    dev_line(p, dx, dy);
    p->cx = dx; p->cy = dy;
}

void gfx_close(struct gfx_path *p)
{
    /* A fill closes every subpath implicitly, so close() only has to put the
     * pen back -- it must NOT emit the closing point, or a closed subpath grows
     * a zero-length edge that the winding walk then counts. */
    p->cx = p->sx; p->cy = p->sy;
    p->open = 0;
}

/* ---- flattening ----
 * Deviation bounds, both standard and both conservative:
 *   quad:  max deviation = |2*C - P0 - P2| / 4
 *   cubic: err^2 = max(ux^2,vx^2) + max(uy^2,vy^2) with
 *          u = 3*C1 - 2*P0 - P3, v = 3*C2 - P0 - 2*P3; flat when err <= 4*tol
 *          (Willcocks). */

static void flat_quad(struct gfx_path *p, int x0, int y0, int cx, int cy,
                      int x1, int y1, int depth)
{
    int dx = 2 * cx - x0 - x1, dy = 2 * cy - y0 - y1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (depth >= MAX_DEPTH || dx + dy <= 4 * p->tol) { dev_line(p, x1, y1); return; }
    int ax = (x0 + cx) >> 1, ay = (y0 + cy) >> 1;
    int bx = (cx + x1) >> 1, by = (cy + y1) >> 1;
    int mx = (ax + bx) >> 1, my = (ay + by) >> 1;
    flat_quad(p, x0, y0, ax, ay, mx, my, depth + 1);
    flat_quad(p, mx, my, bx, by, x1, y1, depth + 1);
}

static void flat_cubic(struct gfx_path *p, int x0, int y0, int c1x, int c1y,
                       int c2x, int c2y, int x1, int y1, int depth)
{
    long long ux = 3LL * c1x - 2LL * x0 - x1, uy = 3LL * c1y - 2LL * y0 - y1;
    long long vx = 3LL * c2x - x0 - 2LL * x1, vy = 3LL * c2y - y0 - 2LL * y1;
    ux *= ux; vx *= vx; uy *= uy; vy *= vy;
    long long err = (ux > vx ? ux : vx) + (uy > vy ? uy : vy);
    long long lim = 16LL * p->tol * p->tol;
    if (depth >= MAX_DEPTH || err <= lim) { dev_line(p, x1, y1); return; }
    int ax = (x0 + c1x) >> 1,  ay = (y0 + c1y) >> 1;
    int bx = (c1x + c2x) >> 1, by = (c1y + c2y) >> 1;
    int cx = (c2x + x1) >> 1,  cy = (c2y + y1) >> 1;
    int dx = (ax + bx) >> 1,   dy = (ay + by) >> 1;
    int ex = (bx + cx) >> 1,   ey = (by + cy) >> 1;
    int mx = (dx + ex) >> 1,   my = (dy + ey) >> 1;
    flat_cubic(p, x0, y0, ax, ay, dx, dy, mx, my, depth + 1);
    flat_cubic(p, mx, my, ex, ey, cx, cy, x1, y1, depth + 1);
}

void gfx_quad_to(struct gfx_path *p, int cx, int cy, int x, int y)
{
    int dcx, dcy, dx, dy;
    gfx_m_apply(&p->m, cx, cy, &dcx, &dcy);
    gfx_m_apply(&p->m, x, y, &dx, &dy);
    if (!p->open) start_sub(p, p->cx, p->cy);
    flat_quad(p, p->cx, p->cy, dcx, dcy, dx, dy, 0);
    p->cx = dx; p->cy = dy;
}

void gfx_cubic_to(struct gfx_path *p, int c1x, int c1y, int c2x, int c2y, int x, int y)
{
    int a1x, a1y, a2x, a2y, dx, dy;
    gfx_m_apply(&p->m, c1x, c1y, &a1x, &a1y);
    gfx_m_apply(&p->m, c2x, c2y, &a2x, &a2y);
    gfx_m_apply(&p->m, x, y, &dx, &dy);
    if (!p->open) start_sub(p, p->cx, p->cy);
    flat_cubic(p, p->cx, p->cy, a1x, a1y, a2x, a2y, dx, dy, 0);
    p->cx = dx; p->cy = dy;
}

/* ---- shape sugar (ordinary path construction, no privileged primitives) ---- */

void gfx_path_rect(struct gfx_path *p, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    gfx_move_to(p, x, y);
    gfx_line_to(p, x + w, y);
    gfx_line_to(p, x + w, y + h);
    gfx_line_to(p, x, y + h);
    gfx_close(p);
}

void gfx_path_rrect4(struct gfx_path *p, int x, int y, int w, int h,
                     int rtl, int rtr, int rbr, int rbl)
{
    if (w <= 0 || h <= 0) return;
    /* Clamp opposing radii against the side they share, the CSS rule: two 60%
     * radii on one edge cannot both be honoured, and scaling them together is
     * what keeps the shape symmetric instead of letting one eat the other. */
    int half = (w < h ? w : h) / 2;
    if (rtl > half) rtl = half;
    if (rtr > half) rtr = half;
    if (rbr > half) rbr = half;
    if (rbl > half) rbl = half;
    if (rtl < 0) rtl = 0;
    if (rtr < 0) rtr = 0;
    if (rbr < 0) rbr = 0;
    if (rbl < 0) rbl = 0;

    int ktl = (int)((long long)rtl * KAPPA >> 16);
    int ktr = (int)((long long)rtr * KAPPA >> 16);
    int kbr = (int)((long long)rbr * KAPPA >> 16);
    int kbl = (int)((long long)rbl * KAPPA >> 16);

    gfx_move_to(p, x + rtl, y);
    gfx_line_to(p, x + w - rtr, y);
    if (rtr) gfx_cubic_to(p, x + w - rtr + ktr, y, x + w, y + rtr - ktr, x + w, y + rtr);
    gfx_line_to(p, x + w, y + h - rbr);
    if (rbr) gfx_cubic_to(p, x + w, y + h - rbr + kbr, x + w - rbr + kbr, y + h,
                          x + w - rbr, y + h);
    gfx_line_to(p, x + rbl, y + h);
    if (rbl) gfx_cubic_to(p, x + rbl - kbl, y + h, x, y + h - rbl + kbl, x, y + h - rbl);
    gfx_line_to(p, x, y + rtl);
    if (rtl) gfx_cubic_to(p, x, y + rtl - ktl, x + rtl - ktl, y, x + rtl, y);
    gfx_close(p);
}

void gfx_path_rrect(struct gfx_path *p, int x, int y, int w, int h, int r)
{
    gfx_path_rrect4(p, x, y, w, h, r, r, r, r);
}

void gfx_path_ellipse(struct gfx_path *p, int cx, int cy, int rx, int ry)
{
    if (rx <= 0 || ry <= 0) return;
    int kx = (int)((long long)rx * KAPPA >> 16);
    int ky = (int)((long long)ry * KAPPA >> 16);
    gfx_move_to(p, cx + rx, cy);
    gfx_cubic_to(p, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    gfx_cubic_to(p, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    gfx_cubic_to(p, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    gfx_cubic_to(p, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    gfx_close(p);
}

void gfx_path_circle(struct gfx_path *p, int cx, int cy, int r)
{
    gfx_path_ellipse(p, cx, cy, r, r);
}

int gfx_path_bounds(const struct gfx_path *p, int *x0, int *y0, int *x1, int *y1)
{
    if (p->npt <= 0) return 0;
    int ax = p->pt[0], ay = p->pt[1], bx = ax, by = ay;
    for (int i = 1; i < p->npt; i++) {
        int x = p->pt[i * 2], y = p->pt[i * 2 + 1];
        if (x < ax) ax = x;
        if (x > bx) bx = x;
        if (y < ay) ay = y;
        if (y > by) by = y;
    }
    if (x0) *x0 = ax >> 8;
    if (y0) *y0 = ay >> 8;
    if (x1) *x1 = (bx + 255) >> 8;
    if (y1) *y1 = (by + 255) >> 8;
    return 1;
}
