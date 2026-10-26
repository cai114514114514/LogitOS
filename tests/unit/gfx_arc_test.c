/* Open Logit -- arcs, SVG arcs, sectors, elliptical and smoothed rounded
 * rects, and gfx_atan2, against independent double-precision references.
 *
 * Coverage is compared with a 16x16 supersampled evaluation of the exact
 * analytic predicate (the bar of tests/unit/gfx_raster_test.c); the SVG arc is
 * compared with a separate double implementation of W3C SVG F.6.5; atan2 with
 * libm. The reference shares no line with the code under test.
 *
 * -DGFX_ARC_NEGCTL (compiled into gfx_arc.c, not here) makes arcs use 180
 * degree cubic pieces: the radial-error budget ("arc-radius") must then FAIL.
 *
 * Build: make test-gfx-arc-shapes */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "gfx.h"

static int fails, checks;

static void ck(int cond, const char *what, const char *detail)
{
    checks++;
    printf("%-4s %s%s%s\n", cond ? "ok" : "FAIL", what,
           detail && *detail ? "  " : "", detail ? detail : "");
    if (!cond) fails++;
}

#define PI 3.14159265358979323846
#define CIRCLE_BUDGET 0.10     /* test-gfx-raster's "every circle matches" bar */

static int   PT[32768 * 2];
static int   SUB[64];
static int   PT2[32768 * 2];
static int   SUB2[64];
static unsigned char M[256 * 256];

static void begin(struct gfx_path *p, int tol)
{
    gfx_path_init(p, PT, 32768, SUB, 64);
    gfx_path_tolerance(p, tol);
}

/* ------------------------------------------------------------- reference -- */
typedef int (*inside_fn)(double x, double y, const double *a);

static double ref_cov(int i, int j, inside_fn f, const double *a)
{
    int in = 0;
    for (int sy = 0; sy < 16; sy++)
        for (int sx = 0; sx < 16; sx++)
            if (f(i + (sx + 0.5) / 16.0, j + (sy + 0.5) / 16.0, a)) in++;
    return in / 256.0;
}

static double worst_err(const unsigned char *m, int w, int h, inside_fn f, const double *a)
{
    double worst = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            double e = fabs(m[j * w + i] / 255.0 - ref_cov(i, j, f, a));
            if (e > worst) worst = e;
        }
    return worst;
}

/* wedge: is (x,y) within `sweep` degrees clockwise (y down) of `start` around c */
static int in_wedge(double dx, double dy, double start, double sweep)
{
    double ang = atan2(dy, dx) * 180.0 / PI - start;
    if (sweep >= 0) {
        ang = fmod(ang, 360.0); if (ang < 0) ang += 360.0;
        return ang <= sweep;
    }
    ang = fmod(-ang, 360.0); if (ang < 0) ang += 360.0;
    return ang <= -sweep;
}
/* a = { cx, cy, r_outer, r_inner, start, sweep } */
static int in_sector(double x, double y, const double *a)
{
    double dx = x - a[0], dy = y - a[1], d2 = dx * dx + dy * dy;
    if (d2 > a[2] * a[2] || d2 < a[3] * a[3]) return 0;
    if (a[5] >= 360 || a[5] <= -360) return 1;
    return in_wedge(dx, dy, a[4], a[5]);
}
/* rounded rect with per-corner ellipses. a = { x,y,w,h, rx0..3, ry0..3 } (tl,tr,br,bl) */
static int in_rrect_ell(double x, double y, const double *a)
{
    double x0 = a[0], y0 = a[1], w = a[2], h = a[3];
    if (x < x0 || y < y0 || x > x0 + w || y > y0 + h) return 0;
    int c = -1;
    double cx = 0, cy = 0;
    if (x < x0 + a[4] && y < y0 + a[8])                   { c = 0; cx = x0 + a[4];     cy = y0 + a[8]; }
    else if (x > x0 + w - a[5] && y < y0 + a[9])          { c = 1; cx = x0 + w - a[5]; cy = y0 + a[9]; }
    else if (x > x0 + w - a[6] && y > y0 + h - a[10])     { c = 2; cx = x0 + w - a[6]; cy = y0 + h - a[10]; }
    else if (x < x0 + a[7] && y > y0 + h - a[11])         { c = 3; cx = x0 + a[7];     cy = y0 + h - a[11]; }
    if (c < 0) return 1;
    double dx = (x - cx) / a[4 + c], dy = (y - cy) / a[8 + c];
    return dx * dx + dy * dy <= 1.0;
}

static double fx(int v) { return v / 256.0; }

/* ------------------------------------------------------------- arc radius -- */
/* Every vertex of the flattened path lies ON the cubic, so its distance from
 * the centre is the cubic's own radial error (plus trig rounding). */
static void test_radius(void)
{
    printf("\n-- arc radius (cubic piece accuracy) --\n");
    static const int sweeps[] = { 90, 200, 270, 360, -150, -360 };
    static const int starts[] = { 0, 37, 200, -100 };
    double worst = 0, worst_rel = 0;
    int bad = 0;
    for (int r = 8; r <= 120; r += 16)
        for (unsigned si = 0; si < sizeof sweeps / sizeof *sweeps; si++)
            for (unsigned ti = 0; ti < sizeof starts / sizeof *starts; ti++) {
                struct gfx_path p;
                begin(&p, GFX_ONE / 64);
                int cx = GFX_PX(150) + 64, cy = GFX_PX(150) + 128;
                gfx_arc(&p, cx, cy, GFX_PX(r), GFX_PX(r), starts[ti] * GFX_ONE + 77,
                        sweeps[si] * GFX_ONE, 0);
                for (int i = 0; i < p.npt; i++) {
                    double dx = fx(p.pt[i * 2] - cx), dy = fx(p.pt[i * 2 + 1] - cy);
                    double e = fabs(sqrt(dx * dx + dy * dy) - r);
                    if (e > worst) worst = e;
                    if (e / r > worst_rel) worst_rel = e / r;
                    if (e > 0.0008 * r + 0.03) bad++;
                }
            }
    char d[160];
    snprintf(d, sizeof d, "worst %.4f px, worst %.5f of r, %d vertices over budget",
             worst, worst_rel, bad);
    ck(bad == 0, "arc-radius", d);
}

/* ---------------------------------------------------------------- coverage -- */
static void test_coverage(void)
{
    printf("\n-- sector and stroked-arc coverage --\n");
    static const double cases[][3] = {          /* r_outer, r_inner, sweep */
        { 20, 0, 90 }, { 30, 12, 130 }, { 18, 6, 270 }, { 24, 10, 360 },
        { 40, 25, -200 }, { 14, 0, 300 },
    };
    static const double frac[] = { 0.25, 0.5 };
    double worst = 0;
    for (unsigned ci = 0; ci < sizeof cases / sizeof *cases; ci++)
        for (unsigned fi = 0; fi < 2; fi++) {
            double ro = cases[ci][0], ri = cases[ci][1], sw = cases[ci][2];
            static const double starts_d[] = { 23.5, 40.5, 61.5, 100.5, 70.5, 152.5 };
            double start = starts_d[ci];
            double cxd = ro + 4 + frac[fi], cyd = ro + 4 + frac[1 - fi];
            int n = (int)(2 * ro + 8 + 1);
            struct gfx_path p;
            begin(&p, GFX_ONE / 64);
            gfx_path_sector(&p, (int)lround(cxd * 256), (int)lround(cyd * 256),
                            (int)lround(ro * 256), (int)lround(ri * 256),
                            (int)lround(start * 256), (int)lround(sw * 256));
            gfx_fill_mask(&p, GFX_NONZERO, M, n, n, 0, 0);
            double a[6] = { cxd, cyd, ro, ri, start, sw };
            double e = worst_err(M, n, n, in_sector, a);
            if (e > worst) worst = e;
            if (e >= CIRCLE_BUDGET) {
                char d[120];
                snprintf(d, sizeof d, "case %u frac %.2f worst %.3f", ci, frac[fi], e);
                ck(0, "sector coverage", d);
            }
        }
    char d[120];
    snprintf(d, sizeof d, "worst pixel error %.3f (budget %.2f)", worst, CIRCLE_BUDGET);
    ck(worst < CIRCLE_BUDGET, "filled sectors match the reference", d);

    worst = 0;
    static const double sc[][3] = { { 22, 5, 100 }, { 16, 4, 250 }, { 30, 7, -140 } };
    for (unsigned ci = 0; ci < 3; ci++)
        for (unsigned fi = 0; fi < 2; fi++) {
            double r = sc[ci][0], wd = sc[ci][1], sw = sc[ci][2], start = 15.0 + 50 * ci;
            double cxd = r + wd + 4 + frac[fi], cyd = r + wd + 4 + frac[1 - fi];
            int n = (int)(2 * (r + wd) + 8 + 1);
            struct gfx_path arc, out;
            begin(&arc, GFX_ONE / 64);
            gfx_arc(&arc, (int)lround(cxd * 256), (int)lround(cyd * 256), (int)lround(r * 256),
                    (int)lround(r * 256), (int)lround(start * 256), (int)lround(sw * 256), 0);
            gfx_path_init(&out, PT2, 32768, SUB2, 64);
            gfx_path_tolerance(&out, GFX_ONE / 64);
            struct gfx_stroke st;
            gfx_zero(&st, sizeof st);
            st.width = (int)lround(wd * 256); st.cap = GFX_CAP_BUTT; st.join = GFX_JOIN_ROUND;
            st.miter_limit = 4 * 65536;
            gfx_stroke_path(&out, &arc, &st);
            gfx_fill_mask(&out, GFX_NONZERO, M, n, n, 0, 0);
            double a[6] = { cxd, cyd, r + wd / 2, r - wd / 2, start, sw };
            double e = worst_err(M, n, n, in_sector, a);
            if (e > worst) worst = e;
        }
    snprintf(d, sizeof d, "worst pixel error %.3f (budget %.2f)", worst, CIRCLE_BUDGET);
    ck(worst < CIRCLE_BUDGET, "stroked arcs match the reference", d);
}

/* ------------------------------------------------------------------ SVG arc -- */
/* Independent double F.6.5. Returns 0 if the arc degenerates. */
struct svgref { double cx, cy, rx, ry, th1, dth, phi; };
static int svg_ref(double x1, double y1, double rx, double ry, double phi_deg, int fa, int fs,
                   double x2, double y2, struct svgref *o)
{
    double phi = phi_deg * PI / 180, cp = cos(phi), sp = sin(phi);
    rx = fabs(rx); ry = fabs(ry);
    double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { rx *= sqrt(lam); ry *= sqrt(lam); }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double co = sqrt(num / den < 0 ? 0 : num / den);
    if (fa == fs) co = -co;
    double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    o->cx = cp * cxp - sp * cyp + (x1 + x2) / 2;
    o->cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    double vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    o->th1 = atan2(uy, ux) * 180 / PI;
    double dth = fmod(atan2(vy, vx) * 180 / PI - o->th1, 360.0);
    if (dth < 0) dth += 360;
    if (!fs && dth > 0) dth -= 360;
    o->dth = dth; o->rx = rx; o->ry = ry; o->phi = phi_deg;
    return 1;
}

static void test_svg(void)
{
    printf("\n-- arc_to_svg against F.6.5 --\n");
    static const struct { double x1, y1, rx, ry, phi, x2, y2; } cs[] = {
        { 10, 50, 30, 30, 0, 60, 50 },
        { 20, 20, 40, 25, 30, 90, 70 },
        { 100, 40, 25, 60, -50, 40, 100 },
        { 30, 30, 5, 5, 0, 100, 30 },           /* radii far too small */
        { 30, 30, 10, 4, 20, 120, 80 },         /* too small, rotated  */
        { 50.5, 60.25, 80, 80, 0, 51.5, 61.25 },/* tiny chord          */
    };
    double worst_end = 0, worst_shape = 0;
    int bad_ang = 0, wc = -1;
    for (unsigned ci = 0; ci < sizeof cs / sizeof *cs; ci++)
        for (int fa = 0; fa < 2; fa++)
            for (int fs = 0; fs < 2; fs++) {
                struct gfx_path p;
                begin(&p, GFX_ONE / 64);
                int x1 = (int)lround(cs[ci].x1 * 256), y1 = (int)lround(cs[ci].y1 * 256);
                int x2 = (int)lround(cs[ci].x2 * 256), y2 = (int)lround(cs[ci].y2 * 256);
                gfx_move_to(&p, x1, y1);
                gfx_arc_to_svg(&p, (int)lround(cs[ci].rx * 256), (int)lround(cs[ci].ry * 256),
                               (int)lround(cs[ci].phi * 256), fa, fs, x2, y2);
                struct svgref o;
                svg_ref(fx(x1), fx(y1), cs[ci].rx, cs[ci].ry, cs[ci].phi, fa, fs, fx(x2), fx(y2), &o);
                /* endpoint */
                double ee = fmax(fabs(fx(p.pt[(p.npt - 1) * 2]) - fx(x2)),
                                 fabs(fx(p.pt[(p.npt - 1) * 2 + 1]) - fx(y2)));
                if (ee > worst_end) worst_end = ee;
                /* every vertex on the reference ellipse and inside its sweep */
                double ph = o.phi * PI / 180, cp = cos(ph), sp = sin(ph);
                for (int i = 0; i < p.npt; i++) {
                    double dx = fx(p.pt[i * 2]) - o.cx, dy = fx(p.pt[i * 2 + 1]) - o.cy;
                    double lx = cp * dx + sp * dy, ly = -sp * dx + cp * dy;
                    double nr = sqrt(lx * lx / (o.rx * o.rx) + ly * ly / (o.ry * o.ry));
                    double e = fabs(nr - 1) * fmax(o.rx, o.ry);
                    if (e > worst_shape) { worst_shape = e; wc = ci * 4 + fa * 2 + fs; }
                    double th = atan2(ly / o.ry, lx / o.rx) * 180 / PI - o.th1;
                    double span = o.dth >= 0 ? o.dth : -o.dth;
                    if (o.dth < 0) th = -th;
                    th = fmod(th, 360.0);
                    if (th < 0) th += 360;
                    if (th > span + 0.2 && th < 360 - 0.2) bad_ang++;
                }
            }
    char d[160];
    snprintf(d, sizeof d, "worst endpoint error %.5f px (limit %.5f)", worst_end, 1.0 / 256);
    ck(worst_end <= 1.0 / 256, "svg arc endpoint", d);
    snprintf(d, sizeof d, "worst vertex distance from the reference ellipse %.4f px (case %d)", worst_shape, wc);
    ck(worst_shape < 0.08, "svg arc lies on the reference ellipse", d);
    snprintf(d, sizeof d, "%d vertices outside the reference sweep", bad_ang);
    ck(bad_ang == 0, "svg arc flags choose the reference arc", d);

    /* zero radius is a line; coincident endpoints draw nothing */
    {
        struct gfx_path p;
        begin(&p, GFX_ONE / 64);
        gfx_move_to(&p, GFX_PX(1), GFX_PX(1));
        gfx_arc_to_svg(&p, 0, GFX_PX(5), 0, 0, 1, GFX_PX(20), GFX_PX(9));
        ck(p.npt == 2 && p.pt[2] == GFX_PX(20) && p.pt[3] == GFX_PX(9), "svg rx=0 degrades to a line", "");
        int before = p.npt;
        gfx_arc_to_svg(&p, GFX_PX(5), GFX_PX(5), 0, 0, 1, GFX_PX(20), GFX_PX(9));
        ck(p.npt == before, "svg coincident endpoints draw nothing", "");
    }
    /* under a path matrix the arc must still end on the transformed endpoint */
    {
        struct gfx_path p;
        struct gfx_matrix m;
        begin(&p, GFX_ONE / 64);
        gfx_m_identity(&m); gfx_m_scale(&m, 2 * 65536, 2 * 65536);
        gfx_path_matrix(&p, &m);
        gfx_move_to(&p, GFX_PX(10), GFX_PX(10));
        gfx_arc_to_svg(&p, GFX_PX(20), GFX_PX(20), 0, 0, 1, GFX_PX(50), GFX_PX(10));
        ck(p.pt[(p.npt - 1) * 2] == GFX_PX(100) && p.pt[(p.npt - 1) * 2 + 1] == GFX_PX(20),
           "svg arc under a scaled matrix ends on the transformed point", "");
    }
}

/* --------------------------------------------------------------------- atan2 -- */
static void test_atan2(void)
{
    printf("\n-- gfx_atan2 --\n");
    double worst = 0;
    for (int i = 0; i < 4096; i++) {
        double ang = (i + 0.37) * 2 * PI / 4096;
        int x = (int)lround(100000 * cos(ang)), y = (int)lround(100000 * sin(ang));
        double want = atan2((double)y, (double)x) * 180 / PI;
        double got = gfx_atan2(y, x) / 256.0;
        double e = fabs(got - want);
        if (e > 180) e = 360 - e;
        if (e > worst) worst = e;
    }
    static const int ax[][2] = { {0,0}, {5,0}, {0,5}, {-5,0}, {0,-5}, {1,1}, {-1,1}, {-1,-1}, {1,-1},
                                 {2147483647, 1}, {-2147483647, -1}, {1, 2147483647} };
    for (unsigned i = 0; i < sizeof ax / sizeof *ax; i++) {
        double want = atan2((double)ax[i][1], (double)ax[i][0]) * 180 / PI;
        double e = fabs(gfx_atan2(ax[i][1], ax[i][0]) / 256.0 - want);
        if (e > 180) e = 360 - e;
        if (e > worst) worst = e;
    }
    char d[100];
    snprintf(d, sizeof d, "max error %.4f deg over 4096 points + axes (limit 0.02)", worst);
    ck(worst <= 0.02, "gfx_atan2 accuracy", d);
}

/* ------------------------------------------------------------ rrects, squircle -- */
static int same_path(const struct gfx_path *a, const struct gfx_path *b)
{
    if (a->npt != b->npt || a->nsub != b->nsub) return 0;
    for (int i = 0; i < a->npt * 2; i++) if (a->pt[i] != b->pt[i]) return 0;
    for (int i = 0; i < a->nsub; i++) if (a->sub[i] != b->sub[i]) return 0;
    return 1;
}

static double max_turn_deg(const struct gfx_path *p)
{
    double worst = 0;
    int n = p->npt;
    for (int i = 0; i < n; i++) {
        int a = (i + n - 1) % n, b = (i + 1) % n;
        double ax = p->pt[i * 2] - p->pt[a * 2], ay = p->pt[i * 2 + 1] - p->pt[a * 2 + 1];
        double bx = p->pt[b * 2] - p->pt[i * 2], by = p->pt[b * 2 + 1] - p->pt[i * 2 + 1];
        if ((ax == 0 && ay == 0) || (bx == 0 && by == 0)) continue;
        double t = fabs(atan2(ax * by - ay * bx, ax * bx + ay * by)) * 180 / PI;
        if (t > worst) worst = t;
    }
    return worst;
}

static void test_rrects(void)
{
    printf("\n-- rrect_ell, squircle --\n");
    static int PA[8192 * 2], SA[16], PB[8192 * 2], SB[16];
    struct gfx_path a, b;
    int equal = 1;
    static const int geo[][5] = { {0, 0, 100, 60, 12}, {10, 20, 64, 64, 32}, {3, 5, 200, 40, 17},
                                  {0, 0, 50, 50, 60} };
    for (unsigned i = 0; i < 4; i++) {
        int x = GFX_PX(geo[i][0]), y = GFX_PX(geo[i][1]), w = GFX_PX(geo[i][2]);
        int h = GFX_PX(geo[i][3]), r = GFX_PX(geo[i][4]) + 37;
        gfx_path_init(&a, PA, 8192, SA, 16); gfx_path_init(&b, PB, 8192, SB, 16);
        gfx_path_squircle(&a, x, y, w, h, r, 0);
        gfx_path_rrect4(&b, x, y, w, h, r, r, r, r);
        if (!same_path(&a, &b)) equal = 0;
        int rr[4] = { r, r / 2, r / 3, r };
        gfx_path_init(&a, PA, 8192, SA, 16); gfx_path_init(&b, PB, 8192, SB, 16);
        gfx_path_rrect_ell(&a, x, y, w, h, rr, rr);
        gfx_path_rrect4(&b, x, y, w, h, rr[0], rr[1], rr[2], rr[3]);
        if (!same_path(&a, &b)) equal = 0;
    }
    ck(equal, "squircle(0) and rrect_ell(rx==ry) are exactly rrect4", "");

    /* elliptical corners against the analytic predicate */
    {
        double worst = 0;
        for (int variant = 0; variant < 3; variant++) {
            int rx[4], ry[4];
            double arx[4], ary[4];
            for (int i = 0; i < 4; i++) {
                arx[i] = 6 + 5 * i + 4 * variant; ary[i] = 14 - 3 * i + 3 * variant;
                rx[i] = (int)lround(arx[i] * 256); ry[i] = (int)lround(ary[i] * 256);
            }
            double ox = 3.25 + 0.25 * variant, oy = 2.5, w = 70.5, h = 52.0;
            struct gfx_path p;
            begin(&p, GFX_ONE / 64);
            gfx_path_rrect_ell(&p, (int)lround(ox * 256), (int)lround(oy * 256),
                               (int)lround(w * 256), (int)lround(h * 256), rx, ry);
            gfx_fill_mask(&p, GFX_NONZERO, M, 80, 64, 0, 0);
            double a2[12] = { ox, oy, w, h, arx[0], arx[1], arx[2], arx[3], ary[0], ary[1], ary[2], ary[3] };
            double e = worst_err(M, 80, 64, in_rrect_ell, a2);
            if (e > worst) worst = e;
        }
        char d[100];
        snprintf(d, sizeof d, "worst pixel error %.3f (budget %.2f)", worst, CIRCLE_BUDGET);
        ck(worst < CIRCLE_BUDGET, "rrect_ell matches the reference", d);
    }
    /* over-large radii get the CSS scale-down, not an overlap */
    {
        int rx[4] = { GFX_PX(40), GFX_PX(40), GFX_PX(40), GFX_PX(40) };
        int ry[4] = { GFX_PX(10), GFX_PX(10), GFX_PX(10), GFX_PX(10) };
        struct gfx_path p;
        begin(&p, GFX_ONE / 64);
        gfx_path_rrect_ell(&p, 0, 0, GFX_PX(60), GFX_PX(40), rx, ry);
        int x0, y0, x1, y1;
        gfx_path_bounds(&p, &x0, &y0, &x1, &y1);
        ck(x0 >= 0 && y0 >= 0 && x1 <= 60 && y1 <= 40 && p.pt[0] <= GFX_PX(30),
           "rrect_ell clamps radii that do not fit", "");
    }

    /* squircle: tangent continuity at s=1, measured on a path built under a
     * 256x matrix so the flattened chords are short enough to see a kink. */
    {
        struct gfx_path p;
        struct gfx_matrix m;
        gfx_m_identity(&m); gfx_m_scale(&m, 256 * 65536, 256 * 65536);
        double worst = 0;
        static const int sm[] = { 256, 160, 80 };
        for (unsigned i = 0; i < 3; i++) {
            begin(&p, GFX_ONE / 64);
            gfx_path_matrix(&p, &m);
            gfx_path_squircle(&p, GFX_PX(2), GFX_PX(3), GFX_PX(120), GFX_PX(90), GFX_PX(24), sm[i]);
            double t = max_turn_deg(&p);
            if (t > worst) worst = t;
        }
        char d[100];
        snprintf(d, sizeof d, "largest turn between flattened segments %.3f deg (limit 0.5)", worst);
        ck(worst <= 0.5, "squircle has no tangent jump at corner/edge joins", d);
        /* the same measure must see a real corner */
        begin(&p, GFX_ONE / 64);
        gfx_path_matrix(&p, &m);
        gfx_path_rect(&p, 0, 0, GFX_PX(10), GFX_PX(10));
        ck(max_turn_deg(&p) > 80, "tangent measure sees a 90 degree corner", "");
    }
    /* corner extent <= min(w,h)/2 and the shape stays in its box */
    {
        int ok = 1;
        static const int rs[] = { 10, 24, 40, 100 };
        for (unsigned i = 0; i < 4; i++)
            for (int s = 0; s <= 256; s += 64) {
                struct gfx_path p;
                begin(&p, GFX_ONE / 64);
                int w = GFX_PX(80), h = GFX_PX(50);
                gfx_path_squircle(&p, GFX_PX(5), GFX_PX(7), w, h, GFX_PX(rs[i]), s);
                int x0, y0, x1, y1;
                gfx_path_bounds(&p, &x0, &y0, &x1, &y1);
                if (x0 < 5 || y0 < 7 || x1 > 85 || y1 > 57) ok = 0;
                /* first point is the end of the top-left corner: x + extent */
                if (p.pt[0] - GFX_PX(5) > GFX_PX(25) || p.pt[1] != GFX_PX(7)) ok = 0;
            }
        ck(ok, "squircle corner extent <= min(w,h)/2, shape inside its box", "");
    }
    /* four-fold symmetry of the filled square */
    {
        struct gfx_path p;
        begin(&p, GFX_ONE / 64);
        gfx_path_squircle(&p, GFX_PX(2), GFX_PX(2), GFX_PX(60), GFX_PX(60), GFX_PX(20), 256);
        gfx_fill_mask(&p, GFX_NONZERO, M, 64, 64, 0, 0);
        int worst = 0;
        for (int j = 0; j < 64; j++)
            for (int i = 0; i < 64; i++) {
                /* the rasteriser samples 4 sub-scanlines but horizontally
                 * exactly, so flips and the transpose agree only to ~1/8 */
                int d1 = abs(M[j * 64 + i] - M[j * 64 + (63 - i)]);
                int d2 = abs(M[j * 64 + i] - M[(63 - j) * 64 + i]);
                int d3 = abs(M[j * 64 + i] - M[i * 64 + j]);
                if (d1 > worst) worst = d1;
                if (d2 > worst) worst = d2;
                if (d3 > worst) worst = d3;
            }
        char d[100];
        snprintf(d, sizeof d, "max asymmetry %d/255", worst);
        ck(worst <= 40, "squircle is symmetric under flips and transpose", d);
    }
}

int main(void)
{
    printf("=== Open Logit: arcs and shapes vs double-precision references ===\n");
    test_radius();
    test_coverage();
    test_svg();
    test_atan2();
    test_rrects();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
