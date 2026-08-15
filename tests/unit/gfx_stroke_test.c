/* Open Logit -- accuracy of the STROKER, against an INDEPENDENT reference.
 *
 * Same bar as the rasterizer suite next door: every stroked shape is compared
 * against a 16x16 supersampled evaluation of its own analytic predicate, and
 * the result is a NUMBER -- worst per-pixel coverage error -- not a picture
 * that looks about right.
 *
 * THE PREDICATE, and getting it right is half the work here:
 *
 *     inside(P)  iff  P is within w/2 of some SEGMENT of the flattened
 *                     source polyline (perpendicular, projection in [0,1])
 *                 or  P is in the join region at some interior vertex
 *                 or  P is in the cap region at an open end
 *
 * Three things about that are deliberate:
 *
 *  1. The distance is to the FLATTENED POLYLINE, never to the ideal curve.
 *     gfx_path.c flattens at record time against the path's tolerance, so a
 *     stroker offsets a polyline and nothing else. Measuring against the true
 *     circle would charge the flattening error to the stroker and fail a
 *     correct implementation for someone else's approximation.
 *  2. ROUND joins and ROUND caps fall out of the distance predicate for free
 *     -- a disc at every vertex is exactly what "distance <= w/2" gives. It
 *     is BUTT that has to be subtracted (hence the t-in-[0,1] clamp on the
 *     band) and MITER that has to be added: a miter adds area OUTSIDE the
 *     distance set, so the predicate is extended with the wedge, computed in
 *     closed form from the two directions and the limit, rather than the
 *     region being excluded from the comparison.
 *  3. The polyline is passed to the reference EXPLICITLY by each test, not
 *     re-derived from the path's storage. The reference must not have to know
 *     the engine's convention for which subpaths are closed.
 *
 * The `const double *a` channel that gfx_raster_test.c's predicates use is
 * four doubles wide and a polyline does not fit in it, so the reference
 * closes over file-scope state instead -- the same thing in_sq_outer() does
 * there, one step further.
 *
 * Frame: pixel (i,j) spans [i,i+1) x [j,j+1) and the 16x16 sample centres sit
 * at +(s+0.5)/16. Get that wrong and a correct rasterizer fails (see the note
 * at gfx_paint_test.c:216 for the time it happened).
 *
 * Build: make test-gfx-stroke   /   negative control: make test-gfx-stroke-negctl
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

/* ============================================================ reference == */

#define RP 24        /* polylines (a dashed path registers one per on-run) */
#define RV 256       /* vertices in each                                   */

static double RX[RP][RV], RY[RP][RV];
static int    RN[RP], RCL[RP], RNP;
static double RH;            /* half width, device px                      */
static int    RJOIN, RCAP;
static double RMITER;        /* miter length / stroke width                */

static void ref_reset(double halfw, int join, int cap, double miter)
{
    RNP = 0; RH = halfw; RJOIN = join; RCAP = cap; RMITER = miter;
}

static void ref_poly_d(const double *xs, const double *ys, int n, int closed)
{
    if (RNP >= RP || n > RV) { printf("     (reference overflow)\n"); return; }
    for (int i = 0; i < n; i++) { RX[RNP][i] = xs[i]; RY[RNP][i] = ys[i]; }
    RN[RNP] = n; RCL[RNP] = closed; RNP++;
}

/* Register a polyline straight out of a built path's storage, in 24.8. The
 * caller says whether it is closed and, if so, has already dropped the
 * repeated first point -- the reference does not guess. */
static void ref_poly_fixed(const int *pt, int n, int closed)
{
    if (RNP >= RP || n > RV) { printf("     (reference overflow)\n"); return; }
    for (int i = 0; i < n; i++) {
        RX[RNP][i] = pt[i * 2] / 256.0;
        RY[RNP][i] = pt[i * 2 + 1] / 256.0;
    }
    RN[RNP] = n; RCL[RNP] = closed; RNP++;
}

static int udir(double ax, double ay, double bx, double by, double *ux, double *uy)
{
    double dx = bx - ax, dy = by - ay, L = sqrt(dx * dx + dy * dy);
    if (L <= 0) return 0;
    *ux = dx / L; *uy = dy / L;
    return 1;
}

/* Perpendicular band of one segment: distance <= RH AND the foot of the
 * perpendicular lands ON the segment. The clamp is what makes a BUTT cap a
 * butt cap. */
static int seg_band(double px, double py, double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy, t, qx, qy;
    if (L2 <= 0) return 0;
    t = ((px - ax) * dx + (py - ay) * dy) / L2;
    if (t < 0 || t > 1) return 0;
    qx = ax + t * dx - px; qy = ay + t * dy - py;
    return qx * qx + qy * qy <= RH * RH;
}

static int in_disc(double px, double py, double cx, double cy)
{
    double dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= RH * RH;
}

static double side_of(double px, double py, double ax, double ay, double bx, double by)
{ return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

static int in_tri(double px, double py, double ax, double ay,
                  double bx, double by, double cx, double cy)
{
    double d0 = side_of(px, py, ax, ay, bx, by);
    double d1 = side_of(px, py, bx, by, cx, cy);
    double d2 = side_of(px, py, cx, cy, ax, ay);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}

/* The join region at a vertex, from the definition of each join.
 *
 * OUTER SIDE. With n = perp(d) = (-dy, dx), a turn with cross(d0,d1) < 0 puts
 * the +n side on the outside of the turn -- that is the side where the two
 * offset bands leave a wedge uncovered, and the only side a join adds
 * anything to. On the inside the bands already overlap.
 *
 * BEVEL is the triangle (V, A0, A1) between the two perpendiculars and the
 * chord. MITER extends it to the quadrilateral (V, A0, T, A1) with the tip
 * T = V + (w/2)(n0+n1)/(1+n0.n1) -- the intersection of the two offset lines
 * -- unless the ratio |T-V|/(w/2) = sqrt(2/(1+n0.n1)) exceeds the limit, in
 * which case the join IS a bevel. ROUND is the whole disc, which the
 * distance predicate would have given anyway. */
static int join_region(double px, double py, double vx, double vy,
                       double d0x, double d0y, double d1x, double d1y)
{
    double n0x = -d0y, n0y = d0x, n1x = -d1y, n1y = d1x;
    double cross = d0x * d1y - d0y * d1x, s, a0x, a0y, a1x, a1y;

    if (RJOIN == GFX_JOIN_ROUND) return in_disc(px, py, vx, vy);
    if (cross == 0) return 0;         /* collinear, or an exact cusp: no area */
    s = cross < 0 ? 1.0 : -1.0;
    a0x = vx + s * RH * n0x; a0y = vy + s * RH * n0y;
    a1x = vx + s * RH * n1x; a1y = vy + s * RH * n1y;

    if (RJOIN == GFX_JOIN_MITER) {
        double c = n0x * n1x + n0y * n1y, den = 1.0 + c;
        if (den > 0 && sqrt(2.0 / den) <= RMITER) {
            double tx = vx + s * RH * (n0x + n1x) / den;
            double ty = vy + s * RH * (n0y + n1y) / den;
            return in_tri(px, py, vx, vy, a0x, a0y, tx, ty) ||
                   in_tri(px, py, vx, vy, tx, ty, a1x, a1y);
        }
    }
    return in_tri(px, py, vx, vy, a0x, a0y, a1x, a1y);
}

/* (dx,dy) points OUT of the path at this end. */
static int cap_region(double px, double py, double vx, double vy,
                      double dx, double dy)
{
    double u, v;
    if (RCAP == GFX_CAP_ROUND) return in_disc(px, py, vx, vy);
    if (RCAP != GFX_CAP_SQUARE) return 0;             /* BUTT adds nothing */
    u = (px - vx) * dx + (py - vy) * dy;
    v = (px - vx) * (-dy) + (py - vy) * dx;
    return u >= 0 && u <= RH && v >= -RH && v <= RH;
}

static int in_stroke(double px, double py, const double *a)
{
    (void)a;
    for (int p = 0; p < RNP; p++) {
        const double *X = RX[p], *Y = RY[p];
        int n = RN[p], closed = RCL[p], nseg, v0, v1;
        if (n <= 0) continue;
        if (n == 1) {
            /* A zero-length subpath: a dot under round caps, an axis-aligned
             * square under square caps, and NOTHING under butt. */
            if (RCAP == GFX_CAP_ROUND && in_disc(px, py, X[0], Y[0])) return 1;
            if (RCAP == GFX_CAP_SQUARE && px >= X[0] - RH && px <= X[0] + RH &&
                py >= Y[0] - RH && py <= Y[0] + RH) return 1;
            continue;
        }
        nseg = closed ? n : n - 1;
        for (int i = 0; i < nseg; i++) {
            int j = (i + 1) % n;
            if (seg_band(px, py, X[i], Y[i], X[j], Y[j])) return 1;
        }
        v0 = closed ? 0 : 1;
        v1 = closed ? n : n - 1;
        for (int v = v0; v < v1; v++) {
            int pi = (v - 1 + n) % n, ci = v % n;
            double d0x, d0y, d1x, d1y;
            if (!udir(X[pi], Y[pi], X[(pi + 1) % n], Y[(pi + 1) % n], &d0x, &d0y)) continue;
            if (!udir(X[ci], Y[ci], X[(ci + 1) % n], Y[(ci + 1) % n], &d1x, &d1y)) continue;
            if (join_region(px, py, X[v], Y[v], d0x, d0y, d1x, d1y)) return 1;
        }
        if (!closed) {
            double dx, dy;
            if (udir(X[0], Y[0], X[1], Y[1], &dx, &dy) &&
                cap_region(px, py, X[0], Y[0], -dx, -dy)) return 1;
            if (udir(X[n - 2], Y[n - 2], X[n - 1], Y[n - 1], &dx, &dy) &&
                cap_region(px, py, X[n - 1], Y[n - 1], dx, dy)) return 1;
        }
    }
    return 0;
}

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
/* -DWORSTLOC names the pixel behind each number. "The worst error moved" says
 * nothing on its own; where it moved to says whether the offset geometry or
 * the rasterizer's vertical sampling is responsible. */
#ifdef WORSTLOC
    { int bi = 0, bj = 0; double bw = 0;
      for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
          double want = ref_cov(i + ox, j + oy, f, a), got = m[j * w + i] / 255.0;
          if (fabs(got - want) > bw) { bw = fabs(got - want); bi = i; bj = j; } }
      if (bw > 0.05) printf("     [worst %.3f at dev (%d,%d): got %.3f want %.3f]\n",
                            bw, bi + ox, bj + oy, m[bj * w + bi] / 255.0,
                            ref_cov(bi + ox, bj + oy, f, a)); }
#endif
    return worst;
}

/* ============================================================== scratch == */

static int SPT[8192 * 2], SSUB[64];
static int OPT[8192 * 2], OSUB[64];
static unsigned char M[256 * 256];

static void src_begin(struct gfx_path *p)
{
    gfx_path_init(p, SPT, 8192, SSUB, 64);
    gfx_path_tolerance(p, GFX_ONE / 64);
}
static void out_begin(struct gfx_path *p)
{
    gfx_path_init(p, OPT, 8192, OSUB, 64);
}

static void st_init(struct gfx_stroke *s, int width, int cap, int join)
{
    s->width = width; s->cap = cap; s->join = join;
    s->miter_limit = 4 << 16;
    s->dash = 0; s->ndash = 0; s->dash_phase = 0;
}

/* Build the outline and rasterize it. Returns gfx_stroke_path's verdict. */
static int stroke_mask(const struct gfx_path *src, const struct gfx_stroke *st,
                       int w, int h, struct gfx_path *out)
{
    out_begin(out);
    if (!gfx_stroke_path(out, src, st)) return 0;
    return gfx_fill_mask(out, GFX_NONZERO, M, w, h, 0, 0);
}

/* One accuracy case: stroke, rasterize, score against whatever the reference
 * has been loaded with. */
static double score(const struct gfx_path *src, const struct gfx_stroke *st,
                    int w, int h, double *mean, int *ok)
{
    struct gfx_path out;
    *ok = stroke_mask(src, st, w, h, &out);
    if (!*ok) return 9.99;
    return worst_err(M, w, h, 0, 0, in_stroke, 0, mean);
}

static void case_err(const char *name, const struct gfx_path *src,
                     const struct gfx_stroke *st, int w, int h, double bar,
                     double *worst_all)
{
    char d[160];
    double mean, e;
    int ok;
    e = score(src, st, w, h, &mean, &ok);
    if (!ok) { ck(0, name, "gfx_stroke_path REFUSED the path"); return; }
    snprintf(d, sizeof d, "worst %.3f mean %.4f", e, mean);
    ck(e < bar, name, d);
    if (worst_all && e > *worst_all) *worst_all = e;
}

static long ink(const unsigned char *m, int n)
{ long s = 0; for (int i = 0; i < n; i++) s += m[i]; return s; }

/* ================================================================ bench ==
 * What stroking costs, split the way bench-gfx splits everything else:
 * building the OUTLINE separately from filling it. That split is the point --
 * gfx_stroke_path() is a path-to-path transform, so a caller that strokes the
 * same geometry every frame can cache the outline and pay only the fill, and
 * a number that welded the two together would hide that. Built only under
 * -DGFX_STROKE_BENCH (make bench-gfx-stroke) so the test binary stays a test.
 */
#ifdef GFX_STROKE_BENCH
#include <time.h>
static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
static void report(const char *what, double us, int n)
{ printf("  %-52s %9.3f us\n", what, us / n); }

static void bench(void)
{
    struct gfx_path src, out;
    struct gfx_stroke st;
    static const int dash[2] = { GFX_PX(8), GFX_PX(4) };
    double t0;
    int i;
    const int N = 20000;

    printf("\n-- what a stroke costs (outline construction, then the fill) --\n");

    src_begin(&src);
    gfx_move_to(&src, GFX_PX(10), GFX_PX(12));
    gfx_line_to(&src, GFX_PX(44), GFX_PX(12));
    gfx_line_to(&src, GFX_PX(44), GFX_PX(46));
    st_init(&st, GFX_PX(6), GFX_CAP_BUTT, GFX_JOIN_MITER);
    t0 = now_us();
    for (i = 0; i < N; i++) { out_begin(&out); gfx_stroke_path(&out, &src, &st); }
    report("outline: an L, 3 pts, miter join", now_us() - t0, N);

    st.join = GFX_JOIN_ROUND; st.cap = GFX_CAP_ROUND;
    t0 = now_us();
    for (i = 0; i < N; i++) { out_begin(&out); gfx_stroke_path(&out, &src, &st); }
    report("outline: the same L, round join + round caps", now_us() - t0, N);

    src_begin(&src);
    gfx_path_circle(&src, GFX_PX(32), GFX_PX(32), GFX_PX(22));
    st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_MITER);
    t0 = now_us();
    for (i = 0; i < N / 10; i++) { out_begin(&out); gfx_stroke_path(&out, &src, &st); }
    report("outline: a flattened circle r=22 (128 vertices)", now_us() - t0, N / 10);
    out_begin(&out);
    gfx_stroke_path(&out, &src, &st);
    t0 = now_us();
    for (i = 0; i < N / 10; i++) gfx_fill_mask(&out, GFX_NONZERO, M, 68, 68, 0, 0);
    report("  ...and filling that outline (68x68 mask)", now_us() - t0, N / 10);

    src_begin(&src);
    gfx_move_to(&src, GFX_PX(5), GFX_PX(12));
    gfx_line_to(&src, GFX_PX(205), GFX_PX(12));
    st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_BEVEL);
    st.dash = dash; st.ndash = 2;
    t0 = now_us();
    for (i = 0; i < N / 10; i++) { out_begin(&out); gfx_stroke_path(&out, &src, &st); }
    report("outline: a 200 px line dashed 8-on 4-off (17 runs)", now_us() - t0, N / 10);
}
#endif

/* ================================================================= main == */

int main(void)
{
    char d[200];
    struct gfx_path src, out;
    struct gfx_stroke st;
    double xs[8], ys[8], worst_all;

    printf("=== Open Logit: stroker vs a 16x supersampled reference ===\n");
    printf("    predicate: distance to the FLATTENED polyline <= w/2, plus the\n"
           "    miter wedge and the square-cap half-square, minus the butt ends\n");

    /* ------------------------------------------------- straight segments -- */
    printf("\n-- a single segment: the three caps --\n");
    {
        worst_all = 0;
        for (int c = 0; c <= 2; c++) {
            static const char *nm[] = { "butt", "round", "square" };
            char t[80];
            src_begin(&src);
            gfx_move_to(&src, GFX_PX(8), GFX_PX(10));
            gfx_line_to(&src, GFX_PX(52), GFX_PX(10));
            xs[0] = 8; ys[0] = 10; xs[1] = 52; ys[1] = 10;
            ref_reset(2.5, GFX_JOIN_BEVEL, c, 4.0);
            ref_poly_d(xs, ys, 2, 0);
            st_init(&st, GFX_PX(5), c, GFX_JOIN_BEVEL);
            snprintf(t, sizeof t, "horizontal line, %s cap", nm[c]);
            case_err(t, &src, &st, 60, 20, 0.14, &worst_all);
        }
        /* A butt cap ENDS at the endpoint: nothing beyond it. That is the
         * assertion the distance predicate on its own would have missed. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(8), GFX_PX(10));
        gfx_line_to(&src, GFX_PX(52), GFX_PX(10));
        st_init(&st, GFX_PX(5), GFX_CAP_BUTT, GFX_JOIN_BEVEL);
        stroke_mask(&src, &st, 60, 20, &out);
        snprintf(d, sizeof d, "col 7 ink %ld, col 8 ink %d",
                 ink(&M[7], 1) + M[10 * 60 + 7], M[10 * 60 + 8]);
        ck(M[10 * 60 + 7] == 0 && M[10 * 60 + 8] == 255,
           "a butt cap stops dead on the endpoint", d);

        /* A square cap extends by HALF the width: the line starts at x=8 with
         * a half-width of 2.5, so the cap runs back to x=5.5 -- pixel 6 whole,
         * pixel 5 half-covered, pixel 4 untouched. Asserting pixel 5 solid
         * would be asserting a cap of 3 px. */
        st_init(&st, GFX_PX(5), GFX_CAP_SQUARE, GFX_JOIN_BEVEL);
        stroke_mask(&src, &st, 60, 20, &out);
        snprintf(d, sizeof d, "px 4/5/6 = %d/%d/%d",
                 M[10 * 60 + 4], M[10 * 60 + 5], M[10 * 60 + 6]);
        ck(M[10 * 60 + 6] == 255 && M[10 * 60 + 5] > 100 && M[10 * 60 + 5] < 155 &&
           M[10 * 60 + 4] == 0,
           "a square cap extends exactly half the width past the endpoint", d);

        for (int c = 0; c <= 2; c++) {
            src_begin(&src);
            gfx_move_to(&src, GFX_PX(6), GFX_PX(6));
            gfx_line_to(&src, GFX_PX(50), GFX_PX(38));
            xs[0] = 6; ys[0] = 6; xs[1] = 50; ys[1] = 38;
            ref_reset(2.0, GFX_JOIN_BEVEL, c, 4.0);
            ref_poly_d(xs, ys, 2, 0);
            st_init(&st, GFX_PX(4), c, GFX_JOIN_BEVEL);
            case_err("diagonal (no axis-aligned edge anywhere)",
                     &src, &st, 60, 46, 0.14, &worst_all);
        }
        snprintf(d, sizeof d, "worst over all six %.3f", worst_all);
        ck(worst_all < 0.14, "straight segments and their caps", d);
    }

    /* ------------------------------------------------------------ joins -- */
    printf("\n-- an L: one join, each kind --\n");
    {
        static const char *jn[] = { "miter", "round", "bevel" };
        xs[0] = 10; ys[0] = 12; xs[1] = 44; ys[1] = 12; xs[2] = 44; ys[2] = 46;
        for (int j = 0; j <= 2; j++) {
            char t[80];
            src_begin(&src);
            gfx_move_to(&src, GFX_PX(10), GFX_PX(12));
            gfx_line_to(&src, GFX_PX(44), GFX_PX(12));
            gfx_line_to(&src, GFX_PX(44), GFX_PX(46));
            ref_reset(3.0, j, GFX_CAP_BUTT, 4.0);
            ref_poly_d(xs, ys, 3, 0);
            st_init(&st, GFX_PX(6), GFX_CAP_BUTT, j);
            snprintf(t, sizeof t, "L, %s join", jn[j]);
            case_err(t, &src, &st, 56, 56, 0.14, 0);
        }
        /* The three joins must actually DIFFER, or all three assertions above
         * could be passing on one shape. A 90-degree miter reaches the outer
         * corner; a bevel cuts it off. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(10), GFX_PX(12));
        gfx_line_to(&src, GFX_PX(44), GFX_PX(12));
        gfx_line_to(&src, GFX_PX(44), GFX_PX(46));
        st_init(&st, GFX_PX(6), GFX_CAP_BUTT, GFX_JOIN_MITER);
        stroke_mask(&src, &st, 56, 56, &out);
        int mcorner = M[9 * 56 + 46];       /* (46.5, 9.5): outside the bevel */
        st_init(&st, GFX_PX(6), GFX_CAP_BUTT, GFX_JOIN_BEVEL);
        stroke_mask(&src, &st, 56, 56, &out);
        int bcorner = M[9 * 56 + 46];
        snprintf(d, sizeof d, "miter %d, bevel %d at the outer corner", mcorner, bcorner);
        ck(mcorner == 255 && bcorner == 0, "miter reaches the corner, bevel does not", d);
    }

    printf("\n-- the miter limit, which is a FALLBACK and not a clip --\n");
    {
        /* A 160-degree turn: the miter ratio is 1/cos(80deg) = 5.76, so a
         * limit of 4 must fall back to bevel and a limit of 8 must not. */
        double ax = 10, ay = 34, bx = 48, by = 34;
        double cx = bx - 34 * cos(20.0 * M_PI / 180.0);
        double cy = by - 34 * sin(20.0 * M_PI / 180.0);
        xs[0] = ax; ys[0] = ay; xs[1] = bx; ys[1] = by; xs[2] = cx; ys[2] = cy;

        src_begin(&src);
        gfx_move_to(&src, (int)(ax * 256), (int)(ay * 256));
        gfx_line_to(&src, (int)(bx * 256), (int)(by * 256));
        gfx_line_to(&src, (int)(cx * 256), (int)(cy * 256));

        ref_reset(2.5, GFX_JOIN_MITER, GFX_CAP_BUTT, 4.0);
        ref_poly_d(xs, ys, 3, 0);
        st_init(&st, GFX_PX(5), GFX_CAP_BUTT, GFX_JOIN_MITER);
        st.miter_limit = 4 << 16;
        case_err("ratio 5.76 over a limit of 4 -> bevel", &src, &st, 60, 46, 0.14, 0);
        long ink4 = ink(M, 60 * 46);

        ref_reset(2.5, GFX_JOIN_MITER, GFX_CAP_BUTT, 8.0);
        ref_poly_d(xs, ys, 3, 0);
        st.miter_limit = 8 << 16;
        case_err("ratio 5.76 under a limit of 8 -> the spike", &src, &st, 60, 46, 0.14, 0);
        long ink8 = ink(M, 60 * 46);

        snprintf(d, sizeof d, "ink %ld with limit 4, %ld with limit 8", ink4, ink8);
        ck(ink8 > ink4 + 255 * 8, "the limit is consulted: the spike is real area", d);

        /* An unset miter_limit is SVG's 4, not zero -- a zero-initialized
         * struct asking for GFX_JOIN_MITER must not silently get bevels. */
        st.miter_limit = 0;
        stroke_mask(&src, &st, 60, 46, &out);
        snprintf(d, sizeof d, "ink %ld, limit-4 ink %ld", ink(M, 60 * 46), ink4);
        ck(ink(M, 60 * 46) == ink4, "miter_limit 0 means SVG's default of 4", d);
    }

    printf("\n-- a Z: two joins, opposite handedness --\n");
    {
        xs[0] = 8; ys[0] = 8; xs[1] = 48; ys[1] = 20;
        xs[2] = 10; ys[2] = 32; xs[3] = 50; ys[3] = 44;
        for (int j = 0; j <= 2; j++) {
            static const char *jn[] = { "miter", "round", "bevel" };
            char t[80];
            src_begin(&src);
            gfx_move_to(&src, GFX_PX(8), GFX_PX(8));
            gfx_line_to(&src, GFX_PX(48), GFX_PX(20));
            gfx_line_to(&src, GFX_PX(10), GFX_PX(32));
            gfx_line_to(&src, GFX_PX(50), GFX_PX(44));
            ref_reset(2.5, j, GFX_CAP_ROUND, 4.0);
            ref_poly_d(xs, ys, 4, 0);
            st_init(&st, GFX_PX(5), GFX_CAP_ROUND, j);
            snprintf(t, sizeof t, "Z, %s join (one turn each way)", jn[j]);
            case_err(t, &src, &st, 60, 52, 0.14, 0);
        }
    }

    /* --------------------------------------------------------- closed -- */
    printf("\n-- a closed triangle: joins at the seam, and a HOLE --\n");
    {
        xs[0] = 12; ys[0] = 10; xs[1] = 52; ys[1] = 20; xs[2] = 20; ys[2] = 48;
        for (int j = 0; j <= 2; j++) {
            static const char *jn[] = { "miter", "round", "bevel" };
            char t[80];
            src_begin(&src);
            gfx_move_to(&src, GFX_PX(12), GFX_PX(10));
            gfx_line_to(&src, GFX_PX(52), GFX_PX(20));
            gfx_line_to(&src, GFX_PX(20), GFX_PX(48));
            gfx_line_to(&src, GFX_PX(12), GFX_PX(10));   /* the closure marker */
            ref_reset(2.5, j, GFX_CAP_BUTT, 4.0);
            ref_poly_d(xs, ys, 3, 1);
            st_init(&st, GFX_PX(5), GFX_CAP_BUTT, j);
            snprintf(t, sizeof t, "closed triangle, %s join", jn[j]);
            case_err(t, &src, &st, 60, 56, 0.14, 0);
        }
        /* Two subpaths, wound oppositely: NONZERO must leave the interior
         * empty. One winding, or two the same way, fills it solid. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(12), GFX_PX(10));
        gfx_line_to(&src, GFX_PX(52), GFX_PX(20));
        gfx_line_to(&src, GFX_PX(20), GFX_PX(48));
        gfx_line_to(&src, GFX_PX(12), GFX_PX(10));
        st_init(&st, GFX_PX(5), GFX_CAP_BUTT, GFX_JOIN_MITER);
        out_begin(&out);
        gfx_stroke_path(&out, &src, &st);
        snprintf(d, sizeof d, "%d subpaths, %d points", out.nsub, out.npt);
        ck(out.nsub == 2, "a closed subpath strokes as two contours", d);
        gfx_fill_mask(&out, GFX_NONZERO, M, 60, 56, 0, 0);
        ck(M[26 * 60 + 27] == 0, "and NONZERO leaves the middle empty", "");
        dump(M, 60, 56, "closed triangle, miter join, width 5");
    }

    printf("\n-- a flattened circle: many joins, and the ring is continuous --\n");
    {
        int n;
        src_begin(&src);
        gfx_path_circle(&src, GFX_PX(32), GFX_PX(32), GFX_PX(22));
        /* gfx_path_ellipse ends its last cubic exactly on its first point, so
         * the stored subpath carries the repeat that marks it closed. Drop it
         * here explicitly rather than making the reference guess. */
        n = src.npt;
        ck(src.pt[0] == src.pt[(n - 1) * 2] && src.pt[1] == src.pt[(n - 1) * 2 + 1],
           "a circle's flattened contour repeats its first point", "");
        ref_reset(2.0, GFX_JOIN_ROUND, GFX_CAP_BUTT, 4.0);
        ref_poly_fixed(src.pt, n - 1, 1);
        st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_ROUND);
        snprintf(d, sizeof d, "%d flattened vertices", n - 1);
        printf("     (%s)\n", d);
        case_err("stroked circle r=22 w=4, round joins", &src, &st, 68, 68, 0.14, 0);

        ref_reset(2.0, GFX_JOIN_MITER, GFX_CAP_BUTT, 4.0);
        ref_poly_fixed(src.pt, n - 1, 1);
        st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_MITER);
        case_err("stroked circle r=22 w=4, miter joins", &src, &st, 68, 68, 0.14, 0);

        /* The ring must not pinch: every row through the circle carries ink
         * on both sides. This is the failure aui's stroked corner had. */
        int gaps = 0;
        for (int j = 12; j < 54; j++) {
            int l = 0, r = 0;
            for (int i = 0; i < 32; i++) l += M[j * 68 + i];
            for (int i = 32; i < 68; i++) r += M[j * 68 + i];
            if (l < 300 || r < 300) gaps++;
        }
        snprintf(d, sizeof d, "%d rows missing ink on one side", gaps);
        ck(gaps == 0, "the stroked ring is continuous all the way round", d);
        ck(M[32 * 68 + 32] == 0, "and hollow in the middle", "");
    }

    printf("\n-- an open flattened arc with round caps --\n");
    {
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(10), GFX_PX(50));
        gfx_cubic_to(&src, GFX_PX(10), GFX_PX(10), GFX_PX(54), GFX_PX(10),
                     GFX_PX(54), GFX_PX(50));
        ref_reset(3.0, GFX_JOIN_ROUND, GFX_CAP_ROUND, 4.0);
        ref_poly_fixed(src.pt, src.npt, 0);
        st_init(&st, GFX_PX(6), GFX_CAP_ROUND, GFX_JOIN_ROUND);
        snprintf(d, sizeof d, "%d flattened vertices", src.npt);
        printf("     (%s)\n", d);
        case_err("open cubic arc, round cap + round join", &src, &st, 64, 60, 0.14, 0);
    }

    /* -------------------------------------------- cusps and reversals -- */
    printf("\n-- a REVERSAL: the outline must not bowtie --\n");
    {
        /* Out and back along the same line. The stroke is one rectangle; the
         * danger is a join that emits a tip on the wrong side, which flips
         * the local winding and punches a hole through the middle of it. */
        xs[0] = 10; ys[0] = 20; xs[1] = 44; ys[1] = 20; xs[2] = 16; ys[2] = 20;
        for (int j = 0; j <= 2; j++) {
            static const char *jn[] = { "miter", "round", "bevel" };
            char t[80];
            src_begin(&src);
            gfx_move_to(&src, GFX_PX(10), GFX_PX(20));
            gfx_line_to(&src, GFX_PX(44), GFX_PX(20));
            gfx_line_to(&src, GFX_PX(16), GFX_PX(20));
            ref_reset(3.0, j, GFX_CAP_BUTT, 4.0);
            ref_poly_d(xs, ys, 3, 0);
            st_init(&st, GFX_PX(6), GFX_CAP_BUTT, j);
            snprintf(t, sizeof t, "exact 180-degree reversal, %s join", jn[j]);
            case_err(t, &src, &st, 56, 32, 0.14, 0);
            snprintf(t, sizeof t, "centre %d, tip %d", M[20 * 56 + 30], M[20 * 56 + 43]);
            ck(M[20 * 56 + 30] == 255 && M[20 * 56 + 43] == 255,
               "the doubled band is SOLID, not holed by a flipped winding", t);
        }
        /* A miter at a reversal is infinite by construction; it must reach
         * the bevel branch through the limit and not through an overflow. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(10), GFX_PX(20));
        gfx_line_to(&src, GFX_PX(44), GFX_PX(20));
        gfx_line_to(&src, GFX_PX(16), GFX_PX(20));
        st_init(&st, GFX_PX(6), GFX_CAP_BUTT, GFX_JOIN_MITER);
        st.miter_limit = 1000 << 16;
        stroke_mask(&src, &st, 56, 32, &out);
        int bx0, by0, bx1, by1;
        gfx_path_bounds(&out, &bx0, &by0, &bx1, &by1);
        snprintf(d, sizeof d, "outline bounds (%d,%d)-(%d,%d)", bx0, by0, bx1, by1);
        ck(bx1 <= 48 && bx0 >= 6, "even with a limit of 1000 the tip stays finite", d);

        /* A NEAR reversal (179 degrees) is the one that actually reaches the
         * arithmetic: cross is tiny but not zero, so the sign test still has
         * to pick a side and the ratio still has to overflow the limit. */
        double ax = 10, ay = 24, bx = 46, by = 24;
        double cx = bx - 34 * cos(1.0 * M_PI / 180.0);
        double cy = by - 34 * sin(1.0 * M_PI / 180.0);
        xs[0] = ax; ys[0] = ay; xs[1] = bx; ys[1] = by; xs[2] = cx; ys[2] = cy;
        src_begin(&src);
        gfx_move_to(&src, (int)(ax * 256), (int)(ay * 256));
        gfx_line_to(&src, (int)(bx * 256), (int)(by * 256));
        gfx_line_to(&src, (int)(cx * 256), (int)(cy * 256));
        ref_reset(3.0, GFX_JOIN_MITER, GFX_CAP_BUTT, 4.0);
        ref_poly_d(xs, ys, 3, 0);
        st_init(&st, GFX_PX(6), GFX_CAP_BUTT, GFX_JOIN_MITER);
        case_err("179-degree near-reversal, miter over the limit", &src, &st, 56, 34, 0.14, 0);

        /* That case carries this suite's worst residual, and it is worth
         * saying whose it is. The two bands are 1 degree apart, so the
         * union's lower edge is a NEARLY HORIZONTAL line at y = 20.607 -- and
         * at 4 sub-scanlines the samples for that pixel row sit at .125,
         * .375, .625, .875, so the edge is resolved to .5 and no offset
         * arithmetic could do better. Raising the count moves the number, so
         * the residual is the rasterizer's vertical sampling and not the
         * stroke geometry; if it were the geometry, subs would not touch it. */
        {
            double m4, m16, e4, e16;
            out_begin(&out);
            gfx_stroke_path(&out, &src, &st);
            gfx_fill_mask_subs(&out, GFX_NONZERO, M, 56, 34, 0, 0, 4);
            e4 = worst_err(M, 56, 34, 0, 0, in_stroke, 0, &m4);
            gfx_fill_mask_subs(&out, GFX_NONZERO, M, 56, 34, 0, 0, 16);
            e16 = worst_err(M, 56, 34, 0, 0, in_stroke, 0, &m16);
            snprintf(d, sizeof d, "worst %.3f at subs=4, %.3f at subs=16 "
                                  "(mean %.4f -> %.4f)", e4, e16, m4, m16);
            ck(e16 < e4 * 0.75,
               "the residual at the sharpest join is SAMPLING, not the offset", d);
        }
    }

    /* --------------------------------------------------------- dashing -- */
    printf("\n-- dashing: each on-run is its own subpath with its own caps --\n");
    {
        /* A 40 px line from x=5 with 8-on / 4-off and no phase. The on-runs
         * are written out LITERALLY rather than computed, so the reference
         * owes nothing to the walker under test:
         *   arc 0..8, 12..20, 24..32, 36..40  ->  x 5..13, 17..25, 29..37, 41..45 */
        static const double on0[4][2] = { { 5, 13 }, { 17, 25 }, { 29, 37 }, { 41, 45 } };
        static const int dash[2] = { GFX_PX(8), GFX_PX(4) };
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(5), GFX_PX(12));
        gfx_line_to(&src, GFX_PX(45), GFX_PX(12));
        ref_reset(2.0, GFX_JOIN_BEVEL, GFX_CAP_BUTT, 4.0);
        for (int k = 0; k < 4; k++) {
            xs[0] = on0[k][0]; ys[0] = 12; xs[1] = on0[k][1]; ys[1] = 12;
            ref_poly_d(xs, ys, 2, 0);
        }
        st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_BEVEL);
        st.dash = dash; st.ndash = 2; st.dash_phase = 0;
        case_err("8-on 4-off, phase 0, butt caps", &src, &st, 52, 20, 0.14, 0);
        stroke_mask(&src, &st, 52, 20, &out);
        snprintf(d, sizeof d, "%d subpaths for 4 on-runs", out.nsub);
        ck(out.nsub == 4, "each on-run is a separate closed outline", d);
        dump(M, 52, 20, "dashed line, 8 on / 4 off");

        /* Same pattern with round caps: every run grows a half-disc at each
         * end, which is what a dotted rule actually looks like. */
        ref_reset(2.0, GFX_JOIN_BEVEL, GFX_CAP_ROUND, 4.0);
        for (int k = 0; k < 4; k++) {
            xs[0] = on0[k][0]; ys[0] = 12; xs[1] = on0[k][1]; ys[1] = 12;
            ref_poly_d(xs, ys, 2, 0);
        }
        st.cap = GFX_CAP_ROUND;
        case_err("8-on 4-off, phase 0, round caps", &src, &st, 52, 20, 0.14, 0);

        /* Phase 3 starts three units into the first ON run, so the first run
         * is short and everything after it slides:
         *   arc 0..5, 9..17, 21..29, 33..40 -> x 5..10, 14..22, 26..34, 38..45 */
        static const double on3[4][2] = { { 5, 10 }, { 14, 22 }, { 26, 34 }, { 38, 45 } };
        ref_reset(2.0, GFX_JOIN_BEVEL, GFX_CAP_BUTT, 4.0);
        for (int k = 0; k < 4; k++) {
            xs[0] = on3[k][0]; ys[0] = 12; xs[1] = on3[k][1]; ys[1] = 12;
            ref_poly_d(xs, ys, 2, 0);
        }
        st.cap = GFX_CAP_BUTT;
        st.dash_phase = GFX_PX(3);
        case_err("dash_phase 3 slides the pattern", &src, &st, 52, 20, 0.14, 0);

        /* A negative phase is a legal offset backwards through the pattern.
         * -3 lands 9 into the 12-unit cycle -- 1 unit into the 4-unit OFF run,
         * so 3 units of OFF remain before anything is drawn:
         *   arc 3..11, 15..23, 27..35, 39..40 -> x 8..16, 20..28, 32..40, 44..45 */
        static const double onm[4][2] = { { 8, 16 }, { 20, 28 }, { 32, 40 }, { 44, 45 } };
        ref_reset(2.0, GFX_JOIN_BEVEL, GFX_CAP_BUTT, 4.0);
        for (int k = 0; k < 4; k++) {
            xs[0] = onm[k][0]; ys[0] = 12; xs[1] = onm[k][1]; ys[1] = 12;
            ref_poly_d(xs, ys, 2, 0);
        }
        st.dash_phase = -GFX_PX(3);
        case_err("a negative dash_phase wraps, it does not clamp", &src, &st, 52, 20, 0.14, 0);
    }

    printf("\n-- a dash that crosses a corner keeps the join inside the run --\n");
    {
        /* An L with a 24-long leg and a 20-long leg, total arc 44, dashed
         * 10-on 5-off. The runs are hand-computed from the arc length:
         *   0..10   -> (6,8)..(16,8)                       one straight run
         *   15..25  -> (21,8)..(30,8)..(30,9)              CROSSES the corner
         *   30..40  -> (30,14)..(30,24)
         * (the corner sits at arc 24, i.e. (30,8), and the second leg runs
         * down to (30,28).) */
        static const int dash[2] = { GFX_PX(10), GFX_PX(5) };
        double r1x[2] = { 6, 16 }, r1y[2] = { 8, 8 };
        double r2x[3] = { 21, 30, 30 }, r2y[3] = { 8, 8, 9 };
        double r3x[2] = { 30, 30 }, r3y[2] = { 14, 24 };
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(6), GFX_PX(8));
        gfx_line_to(&src, GFX_PX(30), GFX_PX(8));
        gfx_line_to(&src, GFX_PX(30), GFX_PX(28));
        ref_reset(2.5, GFX_JOIN_MITER, GFX_CAP_BUTT, 4.0);
        ref_poly_d(r1x, r1y, 2, 0);
        ref_poly_d(r2x, r2y, 3, 0);
        ref_poly_d(r3x, r3y, 2, 0);
        st_init(&st, GFX_PX(5), GFX_CAP_BUTT, GFX_JOIN_MITER);
        st.dash = dash; st.ndash = 2;
        case_err("a run spanning the corner is one subpath with a join",
                 &src, &st, 40, 36, 0.14, 0);
        stroke_mask(&src, &st, 40, 36, &out);
        snprintf(d, sizeof d, "%d subpaths", out.nsub);
        ck(out.nsub == 3, "three on-runs, three outlines", d);
    }

    /* ------------------------------------------------------ degenerates -- */
    printf("\n-- degenerates --\n");
    {
        /* A zero-length subpath. gfx_move_to alone records nothing (the
         * subpath opens on the first segment), so this is move_to + a line_to
         * to the same place -- which is exactly what a real caller emits for
         * a lone point. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(20), GFX_PX(20));
        gfx_line_to(&src, GFX_PX(20), GFX_PX(20));
        snprintf(d, sizeof d, "%d subpath(s), %d point(s)", src.nsub, src.npt);
        ck(src.nsub == 1 && src.npt == 1, "a coincident line_to leaves a 1-point subpath", d);

        xs[0] = 20; ys[0] = 20;
        ref_reset(4.0, GFX_JOIN_BEVEL, GFX_CAP_ROUND, 4.0);
        ref_poly_d(xs, ys, 1, 0);
        st_init(&st, GFX_PX(8), GFX_CAP_ROUND, GFX_JOIN_BEVEL);
        case_err("round cap on a zero-length subpath is a DOT", &src, &st, 40, 40, 0.14, 0);

        ref_reset(4.0, GFX_JOIN_BEVEL, GFX_CAP_SQUARE, 4.0);
        ref_poly_d(xs, ys, 1, 0);
        st.cap = GFX_CAP_SQUARE;
        case_err("square cap on a zero-length subpath is a square", &src, &st, 40, 40, 0.14, 0);

        st.cap = GFX_CAP_BUTT;
        gfx_zero(M, 40 * 40);
        int r = stroke_mask(&src, &st, 40, 40, &out);
        snprintf(d, sizeof d, "returned %d, ink %ld, %d points emitted",
                 r, ink(M, 40 * 40), out.npt);
        ck(r == 1 && ink(M, 40 * 40) == 0 && out.npt == 0,
           "butt cap on a zero-length subpath draws NOTHING, and says so with success", d);

        /* Two coincident points inside a longer path, and a segment one 24.8
         * unit long -- the case where a truncating length would make the
         * "unit" normal 41% too long and push the offset off by pixels. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(8), GFX_PX(12));
        gfx_line_to(&src, GFX_PX(28), GFX_PX(12));
        gfx_line_to(&src, GFX_PX(28) + 1, GFX_PX(12) + 1);   /* 1/256 px long */
        gfx_line_to(&src, GFX_PX(48), GFX_PX(12));
        xs[0] = 8; ys[0] = 12;
        xs[1] = 28; ys[1] = 12;
        xs[2] = 28 + 1 / 256.0; ys[2] = 12 + 1 / 256.0;
        xs[3] = 48; ys[3] = 12;
        ref_reset(3.0, GFX_JOIN_MITER, GFX_CAP_BUTT, 4.0);
        ref_poly_d(xs, ys, 4, 0);
        st_init(&st, GFX_PX(6), GFX_CAP_BUTT, GFX_JOIN_MITER);
        case_err("a segment 1/256 px long does not distort the offset",
                 &src, &st, 56, 24, 0.14, 0);

        /* Thin strokes must SURVIVE as reduced coverage, not vanish. Half a
         * pixel and a quarter of a pixel are exact at the default 4
         * sub-scanlines. */
        for (int w = 128; w >= 64; w >>= 1) {
            char t[80];
            src_begin(&src);
            gfx_move_to(&src, GFX_PX(6), GFX_PX(10) + 128);
            gfx_line_to(&src, GFX_PX(46), GFX_PX(10) + 128);
            xs[0] = 6; ys[0] = 10.5; xs[1] = 46; ys[1] = 10.5;
            ref_reset(w / 512.0, GFX_JOIN_BEVEL, GFX_CAP_BUTT, 4.0);
            ref_poly_d(xs, ys, 2, 0);
            st_init(&st, w, GFX_CAP_BUTT, GFX_JOIN_BEVEL);
            snprintf(t, sizeof t, "width %.3f px survives as coverage", w / 256.0);
            case_err(t, &src, &st, 52, 20, 0.14, 0);
            snprintf(t, sizeof t, "width %.3f px: centre coverage %d",
                     w / 256.0, M[10 * 52 + 26]);
            ck(M[10 * 52 + 26] > 0, "a sub-pixel stroke does not vanish", t);
        }

        /* An EIGHTH of a pixel is where the RASTERIZER's vertical sampling
         * becomes the limit, and it is worth being precise about whose limit
         * it is. At 4 sub-scanlines the samples sit at y+0.125/0.375/0.625/
         * 0.875; a band from 10.4375 to 10.5625 falls between two of them and
         * is missed entirely. That is exactly the trade gfx.h's GFX_SUBS
         * comment describes for a thin horizontal bar, and the answer is the
         * per-call sub-scanline count, not a fudge in the stroker: at 16 the
         * same band catches two samples and comes out at 0.125 to the pixel.
         * The stroker's own contract -- an outline with real area -- is
         * checked on the outline itself below. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(6), GFX_PX(10) + 128);
        gfx_line_to(&src, GFX_PX(46), GFX_PX(10) + 128);
        xs[0] = 6; ys[0] = 10.5; xs[1] = 46; ys[1] = 10.5;
        ref_reset(32 / 512.0, GFX_JOIN_BEVEL, GFX_CAP_BUTT, 4.0);
        ref_poly_d(xs, ys, 2, 0);
        st_init(&st, 32, GFX_CAP_BUTT, GFX_JOIN_BEVEL);
        out_begin(&out);
        gfx_stroke_path(&out, &src, &st);
        gfx_fill_mask(&out, GFX_NONZERO, M, 52, 20, 0, 0);
        int hair4 = M[10 * 52 + 26];
        gfx_fill_mask_subs(&out, GFX_NONZERO, M, 52, 20, 0, 0, 16);
        double mean;
        double e = worst_err(M, 52, 20, 0, 0, in_stroke, 0, &mean);
        snprintf(d, sizeof d, "coverage %d at subs=4, %d at subs=16 (want 32); "
                              "worst %.3f mean %.4f at 16",
                 hair4, M[10 * 52 + 26], e, mean);
        ck(e < 0.14 && M[10 * 52 + 26] > 0,
           "width 0.125 px is the RASTERIZER's sampling limit, not the stroker's", d);

        /* One 24.8 unit wide -- the smallest width that is not a refusal. No
         * sub-scanline count this engine allows (GFX_MAX_SUBS is 32, i.e. a
         * sample every 8/256 px) can show a band 2/256 px tall, so the
         * assertion belongs on the OUTLINE, which is what this file actually
         * produces. The half-width rounds UP: rounding down would give h = 0,
         * an outline of zero area, and a stroke that silently disappeared. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(6), GFX_PX(10));
        gfx_line_to(&src, GFX_PX(46), GFX_PX(10));
        st_init(&st, 1, GFX_CAP_BUTT, GFX_JOIN_BEVEL);
        out_begin(&out);
        r = gfx_stroke_path(&out, &src, &st);
        snprintf(d, sizeof d, "returned %d, %d points, y = %d and %d (centre %d)",
                 r, out.npt, out.pt[1], out.pt[5], GFX_PX(10));
        ck(r == 1 && out.npt == 4 &&
           out.pt[1] == GFX_PX(10) + 1 && out.pt[5] == GFX_PX(10) - 1,
           "width 1/256 px still yields an outline with area", d);
    }

    /* ------------------------------------------------------- capacity -- */
    printf("\n-- capacity: the out-path is the caller's, and it refuses --\n");
    {
        /* An L with bevel joins and butt caps is EXACTLY 9 points, and the
         * breakdown is worth stating because it is the contract a caller
         * sizes its out-path from: one offset point at each end of each side
         * (4), plus the single interior vertex, which costs 3 on the INNER
         * side (A0, the vertex, A1 -- see gfx_stroke.c on why the inner
         * contour is routed through the vertex) and 2 on the outer side for a
         * bevel. A butt cap costs nothing: the polygon edge between the two
         * sides IS the cap. Miter adds one point per outer join, round adds
         * as many as the tolerance asks for. */
        static int p9[9 * 2], s9[4];
        static int p8[8 * 2], s8[4];
        struct gfx_path o9, o8;

        src_begin(&src);
        gfx_move_to(&src, GFX_PX(10), GFX_PX(12));
        gfx_line_to(&src, GFX_PX(44), GFX_PX(12));
        gfx_line_to(&src, GFX_PX(44), GFX_PX(46));
        st_init(&st, GFX_PX(6), GFX_CAP_BUTT, GFX_JOIN_BEVEL);

        gfx_path_init(&o9, p9, 9, s9, 4);
        int r9 = gfx_stroke_path(&o9, &src, &st);
        snprintf(d, sizeof d, "returned %d, %d/%d points, overflow %d",
                 r9, o9.npt, 9, o9.overflow);
        ck(r9 == 1 && !o9.overflow && o9.npt == 9, "9 points is exactly enough", d);

        gfx_path_init(&o8, p8, 8, s8, 4);
        int r8 = gfx_stroke_path(&o8, &src, &st);
        snprintf(d, sizeof d, "returned %d, overflow %d", r8, o8.overflow);
        ck(r8 == 0 && o8.overflow, "8 is exactly not enough, and it REFUSES", d);
        gfx_zero(M, 56 * 56);
        ck(gfx_fill_mask(&o8, GFX_NONZERO, M, 56, 56, 0, 0) == 0,
           "and filling the truncated outline is refused too", "");
        ck(ink(M, 56 * 56) == 0, "nothing was drawn", "");

        /* The subpath table is the other half of the storage, and a closed
         * source needs two entries where an open one needs one. */
        static int pc[64 * 2], sc[1];
        struct gfx_path oc;
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(12), GFX_PX(10));
        gfx_line_to(&src, GFX_PX(52), GFX_PX(20));
        gfx_line_to(&src, GFX_PX(20), GFX_PX(48));
        gfx_line_to(&src, GFX_PX(12), GFX_PX(10));
        gfx_path_init(&oc, pc, 64, sc, 1);
        int rc = gfx_stroke_path(&oc, &src, &st);
        snprintf(d, sizeof d, "returned %d, overflow %d, nsub %d", rc, oc.overflow, oc.nsub);
        ck(rc == 0 && oc.overflow,
           "one subpath slot for a closed stroke's two contours is refused", d);
    }

    /* ------------------------------------------------------- refusals -- */
    printf("\n-- refusals, each one watched --\n");
    {
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(10), GFX_PX(10));
        gfx_line_to(&src, GFX_PX(40), GFX_PX(10));

        st_init(&st, 0, GFX_CAP_BUTT, GFX_JOIN_BEVEL);
        out_begin(&out);
        ck(gfx_stroke_path(&out, &src, &st) == 0 && out.npt == 0,
           "width 0 is refused", "");
        st_init(&st, -GFX_PX(2), GFX_CAP_BUTT, GFX_JOIN_BEVEL);
        out_begin(&out);
        ck(gfx_stroke_path(&out, &src, &st) == 0, "a negative width is refused", "");

        st_init(&st, GFX_PX(4), 99, GFX_JOIN_BEVEL);
        out_begin(&out);
        ck(gfx_stroke_path(&out, &src, &st) == 0 && out.npt == 0,
           "an out-of-range cap is refused, not read as butt", "");
        st_init(&st, GFX_PX(4), GFX_CAP_BUTT, -1);
        out_begin(&out);
        ck(gfx_stroke_path(&out, &src, &st) == 0 && out.npt == 0,
           "an out-of-range join is refused, not read as bevel", "");

        /* A malformed dash pattern must NOT quietly stroke solid: a caller
         * that asked for dashes and got a continuous line has the plausible
         * wrong picture this engine is loud about. */
        static const int odd[3] = { GFX_PX(4), GFX_PX(2), GFX_PX(4) };
        static const int neg[2] = { GFX_PX(4), -GFX_PX(2) };
        static const int zero[2] = { 0, 0 };
        st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_BEVEL);
        st.dash = odd; st.ndash = 3;
        out_begin(&out);
        ck(gfx_stroke_path(&out, &src, &st) == 0 && out.npt == 0,
           "an odd-length dash array is refused", "");
        st.dash = neg; st.ndash = 2;
        out_begin(&out);
        ck(gfx_stroke_path(&out, &src, &st) == 0, "a negative dash length is refused", "");
        st.dash = zero; st.ndash = 2;
        out_begin(&out);
        ck(gfx_stroke_path(&out, &src, &st) == 0,
           "an all-zero dash pattern is refused (it would never advance)", "");

        /* An empty pattern with a NULL pointer is SOLID, which is the
         * documented meaning and not a malformed input. */
        st.dash = 0; st.ndash = 0;
        out_begin(&out);
        ck(gfx_stroke_path(&out, &src, &st) == 1 && out.npt == 4,
           "a NULL dash strokes solid", "");

        /* An overflowed source cannot be stroked: its polyline closes across
         * the middle of the shape, and offsetting that is a confident wrong
         * answer. */
        static int tiny[6 * 2], tsub[2];
        struct gfx_path q;
        gfx_path_init(&q, tiny, 6, tsub, 2);
        gfx_path_tolerance(&q, GFX_ONE / 64);
        gfx_path_circle(&q, GFX_PX(20), GFX_PX(20), GFX_PX(18));
        ck(q.overflow, "the truncated source says so", "");
        out_begin(&out);
        ck(gfx_stroke_path(&out, &q, &st) == 0 && out.npt == 0,
           "and stroking it is refused", "");

        /* An out-path that is already poisoned stays refused rather than
         * being appended to. */
        out_begin(&out);
        out.overflow = 1;
        ck(gfx_stroke_path(&out, &src, &st) == 0, "an already-overflowed out is refused", "");
    }

    /* ---------------------------------------------- the closure convention -- */
    printf("\n-- what marks a subpath closed (and the gfx_path_rect trap) --\n");
    {
        /* Same three vertices, twice: without the repeated first point it is
         * an open polyline with two caps, with it a closed contour with three
         * joins. The stroker has nothing else to go on -- gfx_close() emits
         * no point and sets no flag -- so this is the whole distinction, and
         * it is asserted here so it cannot drift silently. */
        src_begin(&src);
        gfx_move_to(&src, GFX_PX(12), GFX_PX(10));
        gfx_line_to(&src, GFX_PX(52), GFX_PX(20));
        gfx_line_to(&src, GFX_PX(20), GFX_PX(48));
        gfx_close(&src);
        int npt_closed_call = src.npt;
        st_init(&st, GFX_PX(5), GFX_CAP_BUTT, GFX_JOIN_MITER);
        out_begin(&out);
        gfx_stroke_path(&out, &src, &st);
        int nsub_after_close = out.nsub;

        src_begin(&src);
        gfx_move_to(&src, GFX_PX(12), GFX_PX(10));
        gfx_line_to(&src, GFX_PX(52), GFX_PX(20));
        gfx_line_to(&src, GFX_PX(20), GFX_PX(48));
        gfx_line_to(&src, GFX_PX(12), GFX_PX(10));
        out_begin(&out);
        gfx_stroke_path(&out, &src, &st);
        snprintf(d, sizeof d, "gfx_close() -> %d src points, %d contours; "
                              "repeated point -> %d contours",
                 npt_closed_call, nsub_after_close, out.nsub);
        ck(npt_closed_call == 3 && nsub_after_close == 1 && out.nsub == 2,
           "closure is the repeated first point; gfx_close() alone is invisible here", d);

        /* THE CONSEQUENCE, asserted rather than only described, because a
         * comment nobody runs is a comment nobody believes. gfx_path_rrect /
         * ellipse / circle end their last curve exactly on their first point,
         * so they carry the marker and stroke closed. gfx_path_rect stops at
         * the fourth corner and leaves the fourth side to gfx_close(), so it
         * does NOT, and its stroke gets two caps at the top-left instead of a
         * join. When gfx_path.c grows the closing line_to (or struct gfx_path
         * grows a closure bit) this assertion FAILS -- which is the point:
         * the fix should have to come here and delete it. */
        src_begin(&src);
        gfx_path_rrect(&src, GFX_PX(6), GFX_PX(6), GFX_PX(40), GFX_PX(30), GFX_PX(8));
        int rr_closed = src.pt[0] == src.pt[(src.npt - 1) * 2] &&
                        src.pt[1] == src.pt[(src.npt - 1) * 2 + 1];
        src_begin(&src);
        gfx_path_rect(&src, GFX_PX(6), GFX_PX(6), GFX_PX(40), GFX_PX(30));
        int rect_closed = src.npt >= 3 && src.pt[0] == src.pt[(src.npt - 1) * 2] &&
                          src.pt[1] == src.pt[(src.npt - 1) * 2 + 1];
        out_begin(&out);
        gfx_stroke_path(&out, &src, &st);
        snprintf(d, sizeof d, "rrect carries it (%d), rect does not (%d) -> "
                              "%d contour(s) for the rect",
                 rr_closed, rect_closed, out.nsub);
        ck(rr_closed == 1 && rect_closed == 0 && out.nsub == 1,
           "KNOWN GAP: gfx_path_rect strokes OPEN; the curved sugar strokes closed", d);
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    printf(fails ? "FAILED\n" : "all checks passed\n");
#ifdef GFX_STROKE_BENCH
    bench();
#endif
    return fails ? 1 : 0;
}
