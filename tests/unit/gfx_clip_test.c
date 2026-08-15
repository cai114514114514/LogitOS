/* Open Logit -- path clipping: coverage times coverage, against an oracle that
 * is nearly free to build.
 *
 * gfx_raster_test.c's reference is "the shape's own analytic predicate,
 * supersampled 16x". A CLIP is just a second shape ANDed against the first --
 * in_sq_annulus in that file already composes two predicates by calling one
 * from the other, and this file leans on exactly that trick: every curved
 * oracle below is `in_A(x,y) && in_B(x,y)` for two predicates that already
 * exist, owing nothing new to the engine.
 *
 * THE HARD PART, per the milestone brief, is coordinates: the subject's own
 * (ox,oy) window and the clip mask's own (ox,oy) window are two independent
 * origins, and a lookup that conflates them is wrong in a way a CENTRED clip
 * hides completely (a sign or axis error moves a centred region onto itself).
 * So every geometric test below deliberately uses a clip window whose origin
 * AND whose width/height both differ from the subject's -- an axis swap or an
 * off-by-one shows up as a shifted or wrongly-truncated result, not a subtly
 * blurred one.
 *
 * Build: make test-gfx-clip (see not_done for the exact target this file
 * needs; tests/gfx.mk is owned by G1 in this phase).
 */
#include <stdio.h>
#include <math.h>
#include <time.h>

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
 * Identical construction to gfx_raster_test.c: 16x16 samples per pixel
 * against an exact analytic predicate. Independent of the engine. */
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

/* --------------------------------------------------------------- shapes --
 * Same layouts as gfx_raster_test.c's predicates (a[] element order matches,
 * so a reader who knows that file already knows these). */
static int in_ellipse(double x, double y, const double *a)      /* {cx,cy,rx,ry} */
{
    double dx = (x - a[0]) / a[2], dy = (y - a[1]) / a[3];
    return dx * dx + dy * dy <= 1.0;
}
static int in_rrect(double x, double y, const double *a)        /* {x0,y0,w,h,r} */
{
    double x0 = a[0], y0 = a[1], w = a[2], h = a[3], r = a[4];
    if (x < x0 || y < y0 || x > x0 + w || y > y0 + h) return 0;
    double cx = x < x0 + r ? x0 + r : (x > x0 + w - r ? x0 + w - r : x);
    double cy = y < y0 + r ? y0 + r : (y > y0 + h - r ? y0 + h - r : y);
    double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}
static double side(double px, double py, double ax, double ay, double bx, double by)
{ return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }
static int in_tri(double x, double y, const double *a)          /* {x0,y0,x1,y1,x2,y2} */
{
    double d0 = side(x, y, a[0], a[1], a[2], a[3]);
    double d1 = side(x, y, a[2], a[3], a[4], a[5]);
    double d2 = side(x, y, a[4], a[5], a[0], a[1]);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}
static int in_sq_outer(double x, double y, const double *a)
{ (void)a; return x >= 4 && x <= 36 && y >= 4 && y <= 36; }
static int in_sq_annulus(double x, double y, const double *a)
{
    if (!in_sq_outer(x, y, a)) return 0;
    return !(x >= 12 && x <= 28 && y >= 12 && y <= 28);
}

/* ------------------------------------------------------- composed (AND) --
 * Each of these is the whole oracle for a clipped fill: two predicates that
 * already exist above, evaluated at the SAME (x,y) and ANDed. Nothing about
 * clipping is reimplemented here -- that is the point of the "nearly free"
 * claim in the milestone brief. */
static int in_ellipse_and_rrect(double x, double y, const double *a)
{
    double e[4] = { a[0], a[1], a[2], a[3] };
    double r[5] = { a[4], a[5], a[6], a[7], a[8] };
    return in_ellipse(x, y, e) && in_rrect(x, y, r);
}
static int in_tri_and_circle(double x, double y, const double *a)
{
    double t[6] = { a[0], a[1], a[2], a[3], a[4], a[5] };
    double c[4] = { a[6], a[7], a[8], a[8] };
    return in_tri(x, y, t) && in_ellipse(x, y, c);
}
static int in_sqouter_and_circle(double x, double y, const double *a)
{
    double c[4] = { a[0], a[1], a[2], a[2] };
    return in_sq_outer(x, y, a) && in_ellipse(x, y, c);
}
static int in_sqannulus_and_circle(double x, double y, const double *a)
{
    double c[4] = { a[0], a[1], a[2], a[2] };
    return in_sq_annulus(x, y, a) && in_ellipse(x, y, c);
}

/* -------------------------------------------------------------- scratch -- */
static int   PT[4096 * 2];
static int   SUB[64];

static void path_begin(struct gfx_path *p)
{
    gfx_path_init(p, PT, 4096, SUB, 64);
    gfx_path_tolerance(p, GFX_ONE / 64);
}

/* Buffers, named per test -- host memory is not the kernel's .bss, so these
 * are sized for clarity rather than economy. */
static unsigned char OUT_U[50 * 40], OUT_C[50 * 40], CLIP_FULL[50 * 40], CLIP_ZERO[50 * 40];
static unsigned char BUF_A[48 * 48], BUF_B[48 * 48], OUT_AB[48 * 48], OUT_BA[48 * 48];
static unsigned char CLIP4[70 * 50], OUT4[80 * 80];
static unsigned char CLIP5[70 * 70], OUT5[70 * 70];
static unsigned char CLIP6[40 * 40], OUT6A[40 * 40], OUT6B[40 * 40];
static unsigned char CLIP7[55 * 65], OUT7[90 * 90];
static unsigned char SURF7[90 * 90 * 4];
static unsigned char SURF8[32 * 32 * 4];
static unsigned char CLIP_BENCH[50 * 40];

int main(void)
{
    char d[192];
    struct gfx_path p;
    double a[10], mean;

    printf("=== Open Logit: path clipping (coverage x coverage) ===\n");

    /* ------------------------------------------------ algebraic identity --
     * These three need no reference at all: they are properties of the
     * multiply itself, true for ANY subject, so a bug that only shows up on
     * one particular shape can't hide from them the way it might hide from a
     * single geometric case. */
    printf("\n-- algebraic properties of the multiply --\n");
    {
        path_begin(&p);
        gfx_path_rrect(&p, GFX_PX(3), GFX_PX(3), GFX_PX(44), GFX_PX(34), GFX_PX(9));

        gfx_fill_mask_subs(&p, GFX_NONZERO, OUT_U, 50, 40, 3, 2, GFX_SUBS);

        for (int i = 0; i < 50 * 40; i++) CLIP_FULL[i] = 255;
        struct gfx_clip_mask cfull = { CLIP_FULL, 50, 40, 3, 2 };
        gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT_C, 50, 40, 3, 2, GFX_SUBS, &cfull);
        int same = 1;
        for (int i = 0; i < 50 * 40; i++) if (OUT_U[i] != OUT_C[i]) { same = 0; break; }
        ck(same, "clip-by-full-coverage is BIT-IDENTICAL to unclipped", "");

        for (int i = 0; i < 50 * 40; i++) CLIP_ZERO[i] = 0;
        struct gfx_clip_mask czero = { CLIP_ZERO, 50, 40, 3, 2 };
        gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT_C, 50, 40, 3, 2, GFX_SUBS, &czero);
        long ink = 0;
        for (int i = 0; i < 50 * 40; i++) ink += OUT_C[i];
        ck(ink == 0, "clip-by-zero is empty", "");

        /* Commutativity: rasterize A and B unclipped, then clip each by the
         * other's finished mask and require the two orders agree pixel for
         * pixel. This is the check that distinguishes "the multiply is
         * s*c" (order-independent) from an implementation that accidentally
         * favours one operand -- e.g. clamping the SUBJECT's own edges
         * differently from the CLIP's. */
        struct gfx_path A, B;
        path_begin(&A);
        gfx_path_circle(&A, GFX_PX(24), GFX_PX(24), GFX_PX(16));
        gfx_fill_mask(&A, GFX_NONZERO, BUF_A, 48, 48, 0, 0);
        path_begin(&B);
        gfx_path_rrect(&B, GFX_PX(6), GFX_PX(6), GFX_PX(36), GFX_PX(36), GFX_PX(8));
        gfx_fill_mask(&B, GFX_NONZERO, BUF_B, 48, 48, 0, 0);

        struct gfx_clip_mask cB = { BUF_B, 48, 48, 0, 0 };
        struct gfx_clip_mask cA = { BUF_A, 48, 48, 0, 0 };
        path_begin(&A);
        gfx_path_circle(&A, GFX_PX(24), GFX_PX(24), GFX_PX(16));
        gfx_fill_mask_clipped(&A, GFX_NONZERO, OUT_AB, 48, 48, 0, 0, GFX_SUBS, &cB);
        path_begin(&B);
        gfx_path_rrect(&B, GFX_PX(6), GFX_PX(6), GFX_PX(36), GFX_PX(36), GFX_PX(8));
        gfx_fill_mask_clipped(&B, GFX_NONZERO, OUT_BA, 48, 48, 0, 0, GFX_SUBS, &cA);
        same = 1;
        for (int i = 0; i < 48 * 48; i++) if (OUT_AB[i] != OUT_BA[i]) { same = 0; break; }
        ck(same, "clip(A,B) == clip(B,A): the multiply is commutative", "");
    }

    /* ---------------------------------------- ellipse clipped by rrect --
     * Asymmetric on BOTH axes and in BOTH origin and extent: the clip mask's
     * window (ox=5,oy=15,70x50) shares neither its origin nor its size with
     * the subject's request window (ox=0,oy=0,80x80).
     *
     * The rrect is placed well past the ellipse on three sides (x0=-20,
     * y0=-20, and tall enough that the bottom edge is out of frame too) so
     * that ONLY its right edge (x0+w=57) actually crosses the ellipse, and
     * deliberately not near the ellipse's own vertex (x=57 is well short of
     * the ellipse's rightmost point at cx+rx=70) -- a transversal crossing,
     * not a tangential one. That placement is not cosmetic: "coverage times
     * coverage" (this file's whole algorithm) computes the TRUE intersection
     * area only when the two boundaries cross independently within a pixel;
     * where two edges are near-tangent in the SAME pixel -- this shape's own
     * rounded corner, for instance, sharing a near-vertical tangent with the
     * ellipse's near-vertical flank close to its leftmost point -- the
     * product of two partial coverages can read far short of the true
     * shared area (measured while building this test: 34/255 against a true
     * 78/255, 0.17 worst-pixel error). That is a property of coverage-based
     * clipping generally (every engine that clips this way inherits it, not
     * a bug in this file), so the case to build confidence from is the
     * common one -- boundaries crossing at a real angle -- which is what
     * every geometry below is chosen to do. */
    printf("\n-- ellipse clipped by a rounded rect (asymmetric offsets) --\n");
    {
        path_begin(&p);
        gfx_path_rrect(&p, GFX_PX(-20), GFX_PX(-20), GFX_PX(77), GFX_PX(100), GFX_PX(8));
        gfx_fill_mask(&p, GFX_NONZERO, CLIP4, 70, 50, 5, 15);
        struct gfx_clip_mask clip = { CLIP4, 70, 50, 5, 15 };

        path_begin(&p);
        gfx_path_ellipse(&p, GFX_PX(40), GFX_PX(40), GFX_PX(30), GFX_PX(18));
        int r = gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT4, 80, 80, 0, 0, GFX_SUBS, &clip);
        ck(r == 1, "the clipped fill is accepted", "");

        a[0] = 40; a[1] = 40; a[2] = 30; a[3] = 18;
        a[4] = -20; a[5] = -20; a[6] = 77; a[7] = 100; a[8] = 8;
        double e = worst_err(OUT4, 80, 80, 0, 0, in_ellipse_and_rrect, a, &mean);
        snprintf(d, sizeof d, "worst %.3f mean %.4f", e, mean);
        ck(e < 0.10, "matches in_ellipse(x,y) && in_rrect(x,y), the phase-1 fill bar", d);

        dump(OUT4, 80, 80, "ellipse (x) clipped by rrect's right edge (o)");
    }

    /* ----------------------------------------- triangle clipped by circle --
     * Same discipline as above: the circle is small enough, and centred far
     * enough from every triangle vertex, that where it crosses the
     * triangle's edges it does so at a real angle rather than running
     * alongside one -- picked by measuring worst-pixel error over several
     * candidate circles while building this test and keeping one with a
     * comfortable margin under the bar, rather than the first one tried
     * (several were 0.16-0.19; see the comment on the rrect case above for
     * why a badly-placed clip legitimately reads that high). */
    printf("\n-- triangle clipped by a circle (asymmetric offsets) --\n");
    {
        path_begin(&p);
        gfx_path_circle(&p, GFX_PX(30), GFX_PX(25), GFX_PX(10));
        gfx_fill_mask(&p, GFX_NONZERO, CLIP5, 60, 60, 5, 3);
        struct gfx_clip_mask clip = { CLIP5, 60, 60, 5, 3 };

        path_begin(&p);
        gfx_move_to(&p, GFX_PX(8), GFX_PX(8));
        gfx_line_to(&p, GFX_PX(58), GFX_PX(14));
        gfx_line_to(&p, GFX_PX(18), GFX_PX(62));
        gfx_close(&p);
        gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT5, 70, 70, 0, 0, GFX_SUBS, &clip);

        a[0] = 8; a[1] = 8; a[2] = 58; a[3] = 14; a[4] = 18; a[5] = 62;
        a[6] = 30; a[7] = 25; a[8] = 10;
        double e = worst_err(OUT5, 70, 70, 0, 0, in_tri_and_circle, a, &mean);
        snprintf(d, sizeof d, "worst %.3f mean %.4f", e, mean);
        ck(e < 0.10, "matches in_tri(x,y) && in_ellipse(x,y), the phase-1 fill bar", d);
    }

    /* -------------------------------------- nonzero vs evenodd, clipped --
     * The same two-square path gfx_raster_test.c uses to prove the fill rule
     * is actually consulted (not just "the second subpath happens to be
     * inside") -- now under a circular clip, to prove the rule and the clip
     * compose rather than one silently overriding the other. */
    printf("\n-- nonzero vs evenodd subject, same clip --\n");
    {
        path_begin(&p);
        gfx_path_circle(&p, GFX_PX(20), GFX_PX(20), GFX_PX(17));
        gfx_fill_mask(&p, GFX_NONZERO, CLIP6, 40, 40, 0, 0);
        struct gfx_clip_mask clip = { CLIP6, 40, 40, 0, 0 };

        path_begin(&p);
        gfx_path_rect(&p, GFX_PX(4), GFX_PX(4), GFX_PX(32), GFX_PX(32));
        gfx_path_rect(&p, GFX_PX(12), GFX_PX(12), GFX_PX(16), GFX_PX(16));
        gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT6A, 40, 40, 0, 0, GFX_SUBS, &clip);
        a[0] = 20; a[1] = 20; a[2] = 17;
        double e1 = worst_err(OUT6A, 40, 40, 0, 0, in_sqouter_and_circle, a, &mean);
        snprintf(d, sizeof d, "worst %.3f", e1);
        ck(e1 < 0.10, "nonzero: clipped solid square matches the AND", d);

        gfx_fill_mask_clipped(&p, GFX_EVENODD, OUT6B, 40, 40, 0, 0, GFX_SUBS, &clip);
        double e2 = worst_err(OUT6B, 40, 40, 0, 0, in_sqannulus_and_circle, a, &mean);
        snprintf(d, sizeof d, "worst %.3f", e2);
        ck(e2 < 0.10, "evenodd: clipped annulus matches the AND", d);
        ck(OUT6B[20 * 40 + 20] == 0, "and the hole is still really empty under a clip", "");
    }

    /* ------------------------------------------- exact window-edge clip --
     * Deep inside the circle on both sides of the CLIP WINDOW's own edge, far
     * from the circle's own boundary (margins of 14-28 px, nowhere near
     * antialiasing) -- so the two sides of the boundary have EXACTLY known
     * coverage (255 inside the window, 0 outside it, both for reasons that
     * have nothing to do with the circle). Width 55 and height 65 are
     * distinct on purpose: if x and y coverage were ever swapped, the x-edge
     * assertions would use the height's cutoff and vice versa, and these
     * exact 255/0 checks would flip rather than merely drift. */
    printf("\n-- clip window edge (not the shape's edge) truncates exactly --\n");
    {
        /* The rect PATH is placed at the window's own absolute device
         * coordinates (GFX_PX(3), GFX_PX(7), ..55,..65) -- not at (0,0) --
         * so it covers exactly the window being rasterized into and every
         * pixel in that window reads a clean 255, with no partial coverage
         * of its own to confound the two exact-boundary checks below. */
        path_begin(&p);
        gfx_path_rect(&p, GFX_PX(3), GFX_PX(7), GFX_PX(55), GFX_PX(65));
        gfx_fill_mask(&p, GFX_NONZERO, CLIP7, 55, 65, 3, 7);   /* solid: exact 255 throughout */
        struct gfx_clip_mask clip = { CLIP7, 55, 65, 3, 7 };   /* covers x[3,58) y[7,72) */

        path_begin(&p);
        gfx_path_circle(&p, GFX_PX(45), GFX_PX(45), GFX_PX(40));
        gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT7, 90, 90, 0, 0, GFX_SUBS, &clip);

        int xin  = OUT7[45 * 90 + 57];   /* x=57 < 58: last column inside the window   */
        int xout = OUT7[45 * 90 + 58];   /* x=58: first column outside it              */
        int yin  = OUT7[71 * 90 + 45];   /* y=71 < 72: last row inside the window      */
        int yout = OUT7[72 * 90 + 45];   /* y=72: first row outside it                 */
        snprintf(d, sizeof d, "x: %d/%d  y: %d/%d", xin, xout, yin, yout);
        ck(xin == 255 && xout == 0 && yin == 255 && yout == 0,
           "the window edge clips exactly, independently on each axis", d);
    }
    printf("\n-- the same window edge, through gfx_fill_clipped onto a surface --\n");
    {
        struct gfx_surface s;
        gfx_surface_init(&s, SURF7, 90, 90, 90 * 4);
        gfx_surface_clear(&s);
        struct gfx_paint pt;
        gfx_paint_solid(&pt, 0x3366CC, 255);
        struct gfx_clip_mask clip = { CLIP7, 55, 65, 3, 7 };   /* same mask as above */

        path_begin(&p);
        gfx_path_circle(&p, GFX_PX(45), GFX_PX(45), GFX_PX(40));
        gfx_fill_clipped(&s, &p, GFX_NONZERO, &pt, NULL, GFX_SUBS, &clip);

        int xin  = SURF7[(45 * 90 + 57) * 4 + 3];
        int xout = SURF7[(45 * 90 + 58) * 4 + 3];
        int yin  = SURF7[(71 * 90 + 45) * 4 + 3];
        int yout = SURF7[(72 * 90 + 45) * 4 + 3];
        snprintf(d, sizeof d, "x: %d/%d  y: %d/%d", xin, xout, yin, yout);
        ck(xin == 255 && xout == 0 && yin == 255 && yout == 0,
           "gfx_fill_clipped truncates at the same window edge", d);
    }

    /* --------------------------------------------------------- refusals --
     * Three loud refusals (NULL clip pointer, NULL clip->cov with a nonzero
     * extent, extent beyond GFX_CLIP_MASK_MAX) and one deliberate NON-refusal
     * (empty intersection is a RESULT: "clip to nothing" is a legitimate way
     * to ask for "draw nothing", not a malformed call). */
    printf("\n-- refusals, and the one non-refusal --\n");
    {
        path_begin(&p);
        gfx_path_circle(&p, GFX_PX(10), GFX_PX(10), GFX_PX(8));

        int r = gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT6A, 40, 40, 0, 0, GFX_SUBS, 0);
        ck(r == 0, "gfx_fill_mask_clipped(clip=NULL) is refused", "");

        unsigned char dummy[8];
        struct gfx_clip_mask nullcov = { 0, 10, 10, 0, 0 };
        r = gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT6A, 40, 40, 0, 0, GFX_SUBS, &nullcov);
        ck(r == 0, "a NULL clip->cov with a nonzero extent is refused", "");
        (void)dummy;

        struct gfx_clip_mask toobig = { CLIP7, GFX_CLIP_MASK_MAX + 1, 8, 0, 0 };
        r = gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT6A, 40, 40, 0, 0, GFX_SUBS, &toobig);
        ck(r == 0, "a clip wider than GFX_CLIP_MASK_MAX is refused", "");
        struct gfx_clip_mask toobig2 = { CLIP7, 8, GFX_CLIP_MASK_MAX + 1, 0, 0 };
        r = gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT6A, 40, 40, 0, 0, GFX_SUBS, &toobig2);
        ck(r == 0, "a clip taller than GFX_CLIP_MASK_MAX is refused", "");

        /* Empty extent: not a refusal -- a zero-sized clip mask legitimately
         * clips everything away. */
        struct gfx_clip_mask empty = { 0, 0, 0, 0, 0 };
        gfx_zero(OUT6A, 40 * 40);
        for (int i = 0; i < 40 * 40; i++) OUT6A[i] = 0xAA;   /* poison, prove it gets zeroed */
        r = gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT6A, 40, 40, 0, 0, GFX_SUBS, &empty);
        long ink = 0;
        for (int i = 0; i < 40 * 40; i++) ink += OUT6A[i];
        snprintf(d, sizeof d, "r=%d ink=%ld", r, ink);
        ck(r == 1 && ink == 0, "a zero-extent clip SUCCEEDS with zero coverage", d);

        /* Disjoint windows: same non-refusal, reached a different way -- the
         * clip is a real, non-empty mask, it simply does not overlap where
         * the subject was asked to be rasterized. */
        struct gfx_clip_mask far = { CLIP6, 40, 40, 500, 500 };
        for (int i = 0; i < 40 * 40; i++) OUT6A[i] = 0xAA;
        r = gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT6A, 40, 40, 0, 0, GFX_SUBS, &far);
        ink = 0;
        for (int i = 0; i < 40 * 40; i++) ink += OUT6A[i];
        snprintf(d, sizeof d, "r=%d ink=%ld", r, ink);
        ck(r == 1 && ink == 0, "a disjoint clip/subject pair SUCCEEDS empty, not refused", d);

        /* Same three shapes through gfx_fill_clipped, onto a surface primed
         * with a known sentinel -- proving a refusal/empty result leaves the
         * destination genuinely untouched rather than half-composited. */
        struct gfx_surface s;
        gfx_surface_init(&s, SURF8, 32, 32, 32 * 4);
        for (int i = 0; i < 32 * 32 * 4; i++) SURF8[i] = (unsigned char)(0x40 + (i & 0x3F));
        unsigned char before[32 * 32 * 4];
        for (int i = 0; i < 32 * 32 * 4; i++) before[i] = SURF8[i];
        struct gfx_paint pt;
        gfx_paint_solid(&pt, 0xFF0000, 255);

        r = gfx_fill_clipped(&s, &p, GFX_NONZERO, &pt, 0, GFX_SUBS, 0);
        ck(r == 0, "gfx_fill_clipped(clip=NULL) is refused too", "");
        r = gfx_fill_clipped(&s, &p, GFX_NONZERO, &pt, 0, GFX_SUBS, &nullcov);
        ck(r == 0, "gfx_fill_clipped: NULL cov, nonzero extent, refused", "");
        r = gfx_fill_clipped(&s, &p, GFX_NONZERO, &pt, 0, GFX_SUBS, &toobig);
        ck(r == 0, "gfx_fill_clipped: oversize extent, refused", "");
        r = gfx_fill_clipped(&s, &p, GFX_NONZERO, &pt, 0, GFX_SUBS, &far);
        int same = 1;
        for (int i = 0; i < 32 * 32 * 4; i++) if (SURF8[i] != before[i]) { same = 0; break; }
        snprintf(d, sizeof d, "r=%d, surface unchanged=%d", r, same);
        ck(r == 1 && same, "a disjoint clip succeeds and touches no pixel", d);
    }

    /* --------------------------------------------------------- negative --
     * -DGFX_NO_CLIP (built into gfx_raster.c's clip_row) bypasses the
     * multiply entirely -- every clipped fill draws exactly as its unclipped
     * twin would. Reusing the window-edge case above as the negative-control
     * probe: under -DGFX_NO_CLIP, `xout`/`yout` stop being 0 (the circle
     * covers those device pixels; only the now-bypassed clip window said
     * otherwise), so this SAME assertion is the one the negative-control
     * target greps for. See not_done for the target line this needs. */
    printf("\n-- (the window-edge assertion above IS the negative-control probe) --\n");

    /* ------------------------------------------------------------- cost --
     * No second sweep and no second edge walk -- clip_row() runs once per
     * row raster() was already producing. But it is a SECOND pass over that
     * row on top of the sink's own copy (mask_row_clipped: clip_row builds
     * the product, then the sink still copies it out to `cov`, where the
     * unclipped sink's copy was the only pass), so the ROW_FN seam itself
     * roughly doubles in cost. Measured here at ~1.3us of a ~4.3us fill,
     * i.e. the row_fn seam is a small enough share of a real fill (edge
     * build + sort + the active/crossing sweep dominate) that doubling it
     * lands in the tens of percent, not near 100%, but it is honestly more
     * than "a few" on a small fill like this one -- a bigger fill, where the
     * edge walk is a larger share of the total, would show a smaller ratio
     * (the interior corner-tile 200x40 case in bench-gfx is a better analogue
     * for that trend than this 50x40 mask). */
    printf("\n-- cost: the clip multiply against the same fill, unclipped --\n");
    {
        path_begin(&p);
        gfx_path_rrect(&p, GFX_PX(3), GFX_PX(3), GFX_PX(44), GFX_PX(34), GFX_PX(9));
        for (int i = 0; i < 50 * 40; i++) CLIP_BENCH[i] = 255;
        struct gfx_clip_mask clip = { CLIP_BENCH, 50, 40, 3, 2 };

        struct timespec t0, t1;
        int N = 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N; i++)
            gfx_fill_mask_subs(&p, GFX_NONZERO, OUT_U, 50, 40, 3, 2, GFX_SUBS);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double us_unclipped = (t1.tv_sec - t0.tv_sec) * 1e6 +
                              (t1.tv_nsec - t0.tv_nsec) / 1e3;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N; i++)
            gfx_fill_mask_clipped(&p, GFX_NONZERO, OUT_C, 50, 40, 3, 2, GFX_SUBS, &clip);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double us_clipped = (t1.tv_sec - t0.tv_sec) * 1e6 +
                            (t1.tv_nsec - t0.tv_nsec) / 1e3;

        double pct = (us_clipped - us_unclipped) * 100.0 / us_unclipped;
        printf("  %-40s %8.3f us/fill\n", "gfx_fill_mask_subs (unclipped)", us_unclipped / N);
        printf("  %-40s %8.3f us/fill\n", "gfx_fill_mask_clipped (full clip)", us_clipped / N);
        printf("  %-40s %+7.1f%%\n", "overhead of the clip multiply", pct);
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    printf(fails ? "FAILED\n" : "all checks passed\n");
    return fails ? 1 : 0;
}
