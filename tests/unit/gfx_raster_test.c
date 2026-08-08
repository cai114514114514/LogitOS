/* Open Logit -- accuracy of the rasterizer, against an INDEPENDENT reference.
 *
 * The bar this project already invented for its toolkit rasterizer, applied to
 * the whole engine: every filled shape is compared against a 16x16 supersampled
 * evaluation of its exact analytic predicate. The reference owes nothing to the
 * code under test -- it does not share a line with it -- and it is computable,
 * which matters because for 2D coverage there is no ffmpeg to diff against the
 * way there is for H.264.
 *
 * "The shape drew" is not a result. The result is a number: worst per-pixel
 * coverage error, per primitive, per parameter.
 *
 * Build: make test-gfx
 */
#include <stdio.h>
#include <math.h>

#include "gfx.h"

static int fails, checks;

static void ck(int cond, const char *what, const char *detail)
{
    checks++;
    printf("%-4s %s%s%s\n", cond ? "ok" : "FAIL", what,
           detail && *detail ? "  " : "", detail ? detail : "");
    if (!cond) fails++;
}

static void dump(const unsigned char *m, int w, int h, const char *title)
{
    printf("     %s (%dx%d)\n", title, w, h);
    for (int j = 0; j < h; j++) {
        printf("     ");
        for (int i = 0; i < w; i++) {
            int v = m[j * w + i];
            putchar(v == 0 ? '.' : (v >= 250 ? '#' : '0' + v * 9 / 255));
        }
        putchar('\n');
    }
}

/* ------------------------------------------------------------- reference --
 * 16x16 samples per pixel against an exact analytic predicate. Slow, obvious,
 * and independent of the engine. */
typedef int (*inside_fn)(double x, double y, const double *a);

static double ref_cov(int i, int j, inside_fn f, const double *a)
{
    int in = 0;
    for (int sy = 0; sy < 16; sy++)
        for (int sx = 0; sx < 16; sx++)
            if (f(i + (sx + 0.5) / 16.0, j + (sy + 0.5) / 16.0, a)) in++;
    return in / 256.0;
}

static double worst_err(const unsigned char *m, int w, int h, int ox, int oy,
                        inside_fn f, const double *a, double *mean_out)
{
    double worst = 0, sum = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            double want = ref_cov(i + ox, j + oy, f, a);
            double got = m[j * w + i] / 255.0;
            double e = fabs(got - want);
            sum += e;
            if (e > worst) worst = e;
        }
    if (mean_out) *mean_out = sum / (w * h);
    return worst;
}

/* predicates -------------------------------------------------------------- */
/* a = { cx, cy, rx, ry } : inside an axis-aligned ellipse */
static int in_ellipse(double x, double y, const double *a)
{
    double dx = (x - a[0]) / a[2], dy = (y - a[1]) / a[3];
    return dx * dx + dy * dy <= 1.0;
}
/* the toolkit's corner tile: ellipse centred on the tile's bottom-right */
static int in_corner(double x, double y, const double *a)
{
    double dx = (a[0] - x) / a[0], dy = (a[1] - y) / a[1];
    return dx * dx + dy * dy <= 1.0;
}
/* a = { w, h, t } : the ring between the outer corner and one inset by t */
static int in_ring(double x, double y, const double *a)
{
    double w = a[0], h = a[1], t = a[2];
    double ox = (w - x) / w, oy = (h - y) / h;
    if (ox * ox + oy * oy > 1.0) return 0;
    /* The inner ellipse shares the OUTER'S CENTRE and differs only in radii.
     * Writing the centre as (w-t, h-t) instead describes a ring that thins
     * toward the diagonal, and this reference would then "prove" a correct
     * rasterizer wrong. */
    double ix = (w - x) / (w - t), iy = (h - y) / (h - t);
    if (x >= t && y >= t && ix * ix + iy * iy <= 1.0) return 0;
    return 1;
}
/* a = { x0,y0, x1,y1, x2,y2 } : inside a triangle (barycentric sign test) */
static double side(double px, double py, double ax, double ay, double bx, double by)
{ return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }
static int in_tri(double x, double y, const double *a)
{
    double d0 = side(x, y, a[0], a[1], a[2], a[3]);
    double d1 = side(x, y, a[2], a[3], a[4], a[5]);
    double d2 = side(x, y, a[4], a[5], a[0], a[1]);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}
/* a = { cx, cy, w, h, r } : a rounded rect */
static int in_rrect(double x, double y, const double *a)
{
    double x0 = a[0], y0 = a[1], w = a[2], h = a[3], r = a[4];
    if (x < x0 || y < y0 || x > x0 + w || y > y0 + h) return 0;
    double cx = x < x0 + r ? x0 + r : (x > x0 + w - r ? x0 + w - r : x);
    double cy = y < y0 + r ? y0 + r : (y > y0 + h - r ? y0 + h - r : y);
    double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}
/* Two concentric squares: nonzero (same winding) fills solid, evenodd holes it. */
static int in_sq_outer(double x, double y, const double *a)
{ (void)a; return x >= 4 && x <= 36 && y >= 4 && y <= 36; }
static int in_sq_annulus(double x, double y, const double *a)
{
    if (!in_sq_outer(x, y, a)) return 0;
    return !(x >= 12 && x <= 28 && y >= 12 && y <= 28);
}

/* -------------------------------------------------------------- scratch -- */
static int   PT[4096 * 2];
static int   SUB[64];
static unsigned char M[256 * 256];
static unsigned char SURF[128 * 128 * 4];

static void path_begin(struct gfx_path *p)
{
    gfx_path_init(p, PT, 4096, SUB, 64);
    gfx_path_tolerance(p, GFX_ONE / 64);
}

int main(void)
{
    char d[160];
    struct gfx_path p;
    double a[8], mean;

    printf("=== Open Logit: rasterizer vs a 16x supersampled reference ===\n");
    printf("    (GFX_SUBS = %d)\n", GFX_SUBS);

    /* ---------------------------------------------------------- circles -- */
    printf("\n-- circles --\n");
    {
        double worst_all = 0;
        for (int r = 3; r <= 48; r += 3) {
            int n = 2 * r + 4;
            path_begin(&p);
            gfx_path_circle(&p, GFX_PX(r + 2), GFX_PX(r + 2), GFX_PX(r));
            gfx_fill_mask(&p, GFX_NONZERO, M, n, n, 0, 0);
            a[0] = r + 2; a[1] = r + 2; a[2] = r; a[3] = r;
            double e = worst_err(M, n, n, 0, 0, in_ellipse, a, &mean);
            if (e > worst_all) worst_all = e;
            if (e >= 0.10) {
                snprintf(d, sizeof d, "r=%d worst %.3f mean %.4f", r, e, mean);
                ck(0, "circle coverage", d);
            }
        }
        snprintf(d, sizeof d, "r = 3..48, worst pixel error %.3f", worst_all);
        ck(worst_all < 0.10, "every circle matches the reference", d);
    }

    /* --------------------------------------------------------- ellipses -- */
    printf("\n-- ellipses (the scaled case: rx != ry) --\n");
    {
        double worst_all = 0;
        int rx[] = { 30, 12, 40, 7 }, ry[] = { 11, 34, 9, 41 };
        for (int k = 0; k < 4; k++) {
            int w = 2 * rx[k] + 4, h = 2 * ry[k] + 4;
            path_begin(&p);
            gfx_path_ellipse(&p, GFX_PX(rx[k] + 2), GFX_PX(ry[k] + 2),
                             GFX_PX(rx[k]), GFX_PX(ry[k]));
            gfx_fill_mask(&p, GFX_NONZERO, M, w, h, 0, 0);
            a[0] = rx[k] + 2; a[1] = ry[k] + 2; a[2] = rx[k]; a[3] = ry[k];
            double e = worst_err(M, w, h, 0, 0, in_ellipse, a, &mean);
            snprintf(d, sizeof d, "%dx%d worst %.3f mean %.4f", rx[k], ry[k], e, mean);
            ck(e < 0.10, "ellipse is an ellipse, not a squashed circle", d);
            if (e > worst_all) worst_all = e;
        }
    }

    /* ------------------------------------------------------- rrect fill -- */
    printf("\n-- rounded rects --\n");
    {
        int R[] = { 0, 2, 6, 12, 24 };
        for (int k = 0; k < 5; k++) {
            path_begin(&p);
            gfx_path_rrect(&p, GFX_PX(3), GFX_PX(3), GFX_PX(70), GFX_PX(54), GFX_PX(R[k]));
            gfx_fill_mask(&p, GFX_NONZERO, M, 76, 60, 0, 0);
            a[0] = 3; a[1] = 3; a[2] = 70; a[3] = 54; a[4] = R[k];
            double e = worst_err(M, 76, 60, 0, 0, in_rrect, a, &mean);
            snprintf(d, sizeof d, "r=%d worst %.3f mean %.4f", R[k], e, mean);
            ck(e < 0.10, "rounded rect matches the reference", d);
        }
    }

    /* ------------------------------------------------------- triangles --
     * A triangle has three arbitrary slopes and no axis-aligned edge, which is
     * where a rasterizer that only ever drew boxes and circles falls over. */
    printf("\n-- triangles (arbitrary slopes) --\n");
    {
        double tri[4][6] = {
            {  2.0,  2.0, 60.0,  8.0, 20.0, 55.0 },
            { 30.0,  1.0, 62.0, 58.0,  1.0, 40.0 },
            {  1.0, 30.0, 62.0, 28.0, 31.0, 33.0 },   /* a sliver */
            {  5.0,  5.0,  5.0, 58.0, 58.0,  5.0 },   /* axis-aligned right angle */
        };
        for (int k = 0; k < 4; k++) {
            path_begin(&p);
            gfx_move_to(&p, (int)(tri[k][0] * 256), (int)(tri[k][1] * 256));
            gfx_line_to(&p, (int)(tri[k][2] * 256), (int)(tri[k][3] * 256));
            gfx_line_to(&p, (int)(tri[k][4] * 256), (int)(tri[k][5] * 256));
            gfx_close(&p);
            gfx_fill_mask(&p, GFX_NONZERO, M, 64, 64, 0, 0);
            double e = worst_err(M, 64, 64, 0, 0, in_tri, tri[k], &mean);
            snprintf(d, sizeof d, "#%d worst %.3f mean %.4f", k, e, mean);
            ck(e < 0.14, "triangle coverage", d);
        }
    }

    /* -------------------------------------------------------- fill rules -- */
    printf("\n-- nonzero vs evenodd --\n");
    {
        /* Two concentric squares wound the SAME way: nonzero -> solid,
         * evenodd -> annulus. Same path, two rules, two different pictures --
         * which is the only way to show the rule is actually consulted. */
        path_begin(&p);
        gfx_path_rect(&p, GFX_PX(4), GFX_PX(4), GFX_PX(32), GFX_PX(32));
        gfx_path_rect(&p, GFX_PX(12), GFX_PX(12), GFX_PX(16), GFX_PX(16));
        gfx_fill_mask(&p, GFX_NONZERO, M, 40, 40, 0, 0);
        double e1 = worst_err(M, 40, 40, 0, 0, in_sq_outer, a, &mean);
        snprintf(d, sizeof d, "worst %.3f", e1);
        ck(e1 < 0.02, "nonzero fills both squares solid", d);
        gfx_fill_mask(&p, GFX_EVENODD, M, 40, 40, 0, 0);
        double e2 = worst_err(M, 40, 40, 0, 0, in_sq_annulus, a, &mean);
        snprintf(d, sizeof d, "worst %.3f", e2);
        ck(e2 < 0.02, "evenodd punches the hole", d);
        ck(M[20 * 40 + 20] == 0, "and the hole is really empty", "");

        /* Reversed inner square: now NONZERO also holes it, because the
         * windings cancel. This separates "the rule is read" from "the second
         * subpath happens to be inside". */
        path_begin(&p);
        gfx_path_rect(&p, GFX_PX(4), GFX_PX(4), GFX_PX(32), GFX_PX(32));
        gfx_move_to(&p, GFX_PX(12), GFX_PX(12));
        gfx_line_to(&p, GFX_PX(12), GFX_PX(28));
        gfx_line_to(&p, GFX_PX(28), GFX_PX(28));
        gfx_line_to(&p, GFX_PX(28), GFX_PX(12));
        gfx_close(&p);
        gfx_fill_mask(&p, GFX_NONZERO, M, 40, 40, 0, 0);
        double e3 = worst_err(M, 40, 40, 0, 0, in_sq_annulus, a, &mean);
        snprintf(d, sizeof d, "worst %.3f", e3);
        ck(e3 < 0.02, "a reversed subpath cancels under nonzero", d);
    }

    /* ------------------------------------------------------ corner tiles -- */
    printf("\n-- the cached corner tiles (what the toolkit draws with) --\n");
    {
        double worst_all = 0;
        for (int r = 4; r <= 32; r += 4) {
            gfx_corner_fill(M, r, r);
            a[0] = r; a[1] = r;
            double e = worst_err(M, r, r, 0, 0, in_corner, a, &mean);
            if (e > worst_all) worst_all = e;
            if (e >= 0.10) {
                snprintf(d, sizeof d, "r=%d worst %.3f", r, e);
                ck(0, "fill corner", d);
            }
        }
        snprintf(d, sizeof d, "r = 4..32, worst %.3f", worst_all);
        ck(worst_all < 0.10, "filled corner tile matches the reference", d);

        gfx_corner_fill(M, 20, 14);
        a[0] = 20; a[1] = 14;
        double e = worst_err(M, 20, 14, 0, 0, in_corner, a, &mean);
        snprintf(d, sizeof d, "worst %.3f", e);
        ck(e < 0.10, "a non-square corner is an ellipse quadrant", d);

        gfx_corner_fill(M, 24, 24);
        dump(M, 24, 24, "fill corner r=24");

        worst_all = 0;
        for (int r = 8; r <= 28; r += 4)
            for (int t = 1; t <= 4; t++) {
                gfx_corner_ring(M, r, r, t);
                a[0] = r; a[1] = r; a[2] = t;
                double er = worst_err(M, r, r, 0, 0, in_ring, a, &mean);
                if (er > worst_all) worst_all = er;
                if (er >= 0.14) {
                    snprintf(d, sizeof d, "r=%d t=%d worst %.3f", r, t, er);
                    ck(0, "ring corner", d);
                }
            }
        snprintf(d, sizeof d, "r = 8..28, t = 1..4, worst %.3f", worst_all);
        ck(worst_all < 0.14, "ring corner tile matches the reference ring", d);

        gfx_corner_ring(M, 24, 24, 3);
        dump(M, 24, 24, "ring corner r=24 t=3");
        /* The ring must be CONTINUOUS. A ring that pinches to nothing mid-arc
         * renders as a rounded box whose corners fade out before they reach the
         * straight edges -- the bug this assertion exists for. */
        int empty_rows = 0;
        for (int j = 0; j < 24; j++) {
            int sum = 0;
            for (int i = 0; i < 24; i++) sum += M[j * 24 + i];
            if (sum < 200) empty_rows++;
        }
        snprintf(d, sizeof d, "%d rows carry less than one pixel of ink", empty_rows);
        ck(empty_rows == 0, "the ring arc is continuous down the whole tile", d);
        int first = 0, last = 0;
        for (int i = 0; i < 24; i++) { first += M[i]; last += M[23 * 24 + i]; }
        snprintf(d, sizeof d, "top row ink %d, bottom row ink %d", first, last);
        ck(first > 400 && last > 400, "the arc meets both straight edges at full weight", d);
    }

    /* ------------------------------------------------------- transforms --
     * The affine transform is applied to PATHS. That is what CSS `transform`
     * needs, and the check that it is real is that a shape drawn through a
     * matrix matches the same shape drawn at the transformed coordinates. */
    printf("\n-- affine transform on paths --\n");
    {
        struct gfx_matrix m;
        gfx_m_identity(&m);
        gfx_m_translate(&m, GFX_PX(10), GFX_PX(6));
        gfx_m_scale(&m, 2 * GFX_MONE, GFX_MONE / 2);
        path_begin(&p);
        gfx_path_matrix(&p, &m);
        gfx_path_circle(&p, GFX_PX(12), GFX_PX(40), GFX_PX(10));
        gfx_fill_mask(&p, GFX_NONZERO, M, 64, 40, 0, 0);
        /* circle r=10 at (12,40) under scale(2, 0.5) then translate(10,6)
         * -> ellipse centred (34, 26) with radii (20, 5). */
        a[0] = 34; a[1] = 26; a[2] = 20; a[3] = 5;
        double e = worst_err(M, 64, 40, 0, 0, in_ellipse, a, &mean);
        snprintf(d, sizeof d, "worst %.3f mean %.4f", e, mean);
        ck(e < 0.10, "scale+translate through the CTM lands where it should", d);

        /* translate(-50%,-50%) centring -- the CSS line's single most-reached
         * transform, 14 of 15 real pages. A 40x20 box centred on (32,20). */
        gfx_m_identity(&m);
        gfx_m_translate(&m, GFX_PX(32), GFX_PX(20));
        gfx_m_translate(&m, -GFX_PX(20), -GFX_PX(10));
        path_begin(&p);
        gfx_path_matrix(&p, &m);
        gfx_path_rect(&p, 0, 0, GFX_PX(40), GFX_PX(20));
        int bx0, by0, bx1, by1;
        gfx_path_bounds(&p, &bx0, &by0, &bx1, &by1);
        snprintf(d, sizeof d, "bounds (%d,%d)-(%d,%d)", bx0, by0, bx1, by1);
        ck(bx0 == 12 && by0 == 10 && bx1 == 52 && by1 == 30,
           "translate(-50%,-50%) centres the box", d);

        /* Rotation: a square rotated 45 degrees is a diamond of the right
         * diagonal, and its AREA is preserved -- which is the property a
         * rasterizer with a broken transform cannot fake. */
        gfx_m_identity(&m);
        gfx_m_translate(&m, GFX_PX(32), GFX_PX(32));
        gfx_m_rotate(&m, 45 * GFX_ONE);
        path_begin(&p);
        gfx_path_matrix(&p, &m);
        gfx_path_rect(&p, -GFX_PX(15), -GFX_PX(15), GFX_PX(30), GFX_PX(30));
        gfx_fill_mask(&p, GFX_NONZERO, M, 64, 64, 0, 0);
        long area = 0;
        for (int i = 0; i < 64 * 64; i++) area += M[i];
        double px = area / 255.0;
        snprintf(d, sizeof d, "%.1f px covered, expected 900", px);
        ck(px > 880 && px < 920, "a rotated square keeps its area", d);
        ck(M[32 * 64 + 32] == 255, "and its centre is solid", "");
        ck(M[16 * 64 + 16] == 0, "and its corner is empty", "");

        struct gfx_matrix inv, back;
        gfx_m_identity(&m);
        gfx_m_translate(&m, GFX_PX(7), -GFX_PX(3));
        gfx_m_rotate(&m, 31 * GFX_ONE);
        gfx_m_scale(&m, 3 * GFX_MONE / 2, 4 * GFX_MONE / 5);
        ck(gfx_m_invert(&inv, &m), "a non-singular matrix inverts", "");
        gfx_m_mul(&back, &m, &inv);
        int ox, oy;
        gfx_m_apply(&back, GFX_PX(123), GFX_PX(-45), &ox, &oy);
        snprintf(d, sizeof d, "(%d,%d) vs (%d,%d)", ox, oy, GFX_PX(123), GFX_PX(-45));
        ck(ox > GFX_PX(123) - 8 && ox < GFX_PX(123) + 8 &&
           oy > GFX_PX(-45) - 8 && oy < GFX_PX(-45) + 8,
           "M * M^-1 is the identity to within 1/32 px", d);
        gfx_m_set(&m, 0, 0, 0, 0, 0, 0);
        ck(!gfx_m_invert(&inv, &m), "a singular matrix is refused, not divided by", "");
    }

    /* ------------------------------------------------------------- clip -- */
    printf("\n-- rectangle clip --\n");
    {
        struct gfx_surface s;
        gfx_surface_init(&s, SURF, 64, 64, 64 * 4);
        gfx_surface_clear(&s);
        struct gfx_paint pt;
        gfx_paint_solid(&pt, 0xFF0000, 255);
        path_begin(&p);
        gfx_path_rect(&p, 0, 0, GFX_PX(64), GFX_PX(64));
        struct gfx_rect cl; cl.x = 10; cl.y = 20; cl.w = 20; cl.h = 8;
        gfx_fill(&s, &p, GFX_NONZERO, &pt, &cl);
        int inside = SURF[(20 * 64 + 10) * 4 + 3], edge = SURF[(27 * 64 + 29) * 4 + 3];
        int out1 = SURF[(19 * 64 + 10) * 4 + 3], out2 = SURF[(20 * 64 + 30) * 4 + 3];
        snprintf(d, sizeof d, "in %d/%d, out %d/%d", inside, edge, out1, out2);
        ck(inside == 255 && edge == 255 && out1 == 0 && out2 == 0,
           "the clip rect is honoured exactly, on all four sides", d);
    }

    /* --------------------------------------------------------- overflow -- */
    printf("\n-- refusals --\n");
    {
        static int tiny[8 * 2];
        static int tsub[2];
        struct gfx_path q;
        gfx_path_init(&q, tiny, 8, tsub, 2);
        gfx_path_tolerance(&q, GFX_ONE / 64);
        gfx_path_circle(&q, GFX_PX(20), GFX_PX(20), GFX_PX(18));
        ck(q.overflow, "a path that outruns its storage says so", "");
        gfx_zero(M, 40 * 40);
        ck(gfx_fill_mask(&q, GFX_NONZERO, M, 40, 40, 0, 0) == 0,
           "and filling it is REFUSED, not drawn truncated", "");
        long ink = 0;
        for (int i = 0; i < 40 * 40; i++) ink += M[i];
        ck(ink == 0, "nothing was drawn", "");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    printf(fails ? "FAILED\n" : "all checks passed\n");
    return fails ? 1 : 0;
}
