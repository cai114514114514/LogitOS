/* ol_ease256_ex against a double reference. Built with -DGFX_NCTX=2 and its
 * own gfx_ctx_hook (it does not link fb.c). */
#include <math.h>
#include <stdio.h>
#include "openlogit_anim.h"

int (*gfx_ctx_hook)(void);
static int cur;
static int hook(void) { return cur; }

static int fails;
#define CHECK(ok, ...) do { if (!(ok)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static double ref(int curve, double x)
{
    const double c1 = 1.70158, c2 = c1 * 1.525, c3 = c1 + 1;
    switch (curve) {
    case OL_LINEAR: return x;
    case OL_EASE_OUT: return x * (2 - x);
    case OL_EASE_INOUT: return x * x * (3 - 2 * x);
    case OL_EASE_OUT_CUBIC: return 1 - pow(1 - x, 3);
    case OL_EASE_IN: return x * x;
    case OL_EASE_IN_CUBIC: return x * x * x;
    case OL_EASE_INOUT_CUBIC: return x < .5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
    case OL_EASE_OUT_QUART: return 1 - pow(1 - x, 4);
    case OL_EASE_OUT_EXPO: return x >= 1 ? 1 : 1 - pow(2, -10 * x);
    case OL_EASE_OUT_BACK: return 1 + c3 * pow(x - 1, 3) + c1 * pow(x - 1, 2);
    case OL_EASE_INOUT_BACK:
        return x < .5 ? pow(2 * x, 2) * ((c2 + 1) * 2 * x - c2) / 2
                      : (pow(2 * x - 2, 2) * ((c2 + 1) * (2 * x - 2) + c2) + 2) / 2;
    }
    return -1000;
}
/* WebKit UnitBezier: Newton (8 steps) then bisection on x, then y. */
static double webkit(double x1, double y1, double x2, double y2, double x)
{
    double cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
    double cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
    double t = x, e = 1e-7;
    for (int i = 0; i < 8; i++) {
        double xx = ((ax * t + bx) * t + cx) * t - x;
        if (fabs(xx) < e) goto done;
        double d = (3 * ax * t + 2 * bx) * t + cx;
        if (fabs(d) < 1e-6) break;
        t -= xx / d;
    }
    double lo = 0, hi = 1;
    t = x;
    while (lo < hi) {
        double xx = ((ax * t + bx) * t + cx) * t;
        if (fabs(xx - x) < e) break;
        if (x > xx) lo = t; else hi = t;
        t = (hi - lo) / 2 + lo;
    }
done:
    return ((ay * t + by) * t + cy) * t;
}
static const char *names[] = {"linear","out","inout","in","bezier","steps","out_cubic","in_cubic",
    "inout_cubic","out_quart","out_expo","out_back","inout_back"};

int main(void)
{
    /* Every analytic curve: error <= 1/256 of full scale, i.e. 1 unit. */
    for (int c = OL_LINEAR; c <= OL_EASE_INOUT_BACK; c++) {
        if (c == OL_CUBIC_BEZIER || c == OL_STEPS) continue;
        double worst = 0;
        for (int t = 0; t <= 256; t++) {
            double d = fabs(ol_ease256_ex(c, t, 0, 0) - ref(c, t / 256.0) * 256);
            if (d > worst) worst = d;
        }
        printf("curve %-10s max err %.3f/256\n", names[c], worst);
        const char *tag = (c == OL_EASE_OUT_BACK || c == OL_EASE_INOUT_BACK) ? "back" : "curve";
        CHECK(worst <= 1.0, "%s %s error %.3f", tag, names[c], worst);
        int e0 = ol_ease256_ex(c, 0, 0, 0), e1 = ol_ease256_ex(c, 256, 0, 0);
        CHECK(e0 == 0 && e1 == 256, "endpoints %s %d %d", names[c], e0, e1);
        CHECK(ol_ease256(c, -5) == e0 && ol_ease256(c, 999) == e1, "clamp %s", names[c]);
    }
    /* Beziers, including overshoot and the CSS presets. */
    static const double P[][4] = {{.25,.1,.25,1},{.42,0,1,1},{0,0,.58,1},{.42,0,.58,1},
                                  {.68,-.55,.27,1.55},{0,1,1,0},{.17,.67,.83,.67},{1,0,0,1}};
    for (unsigned i = 0; i < sizeof P / sizeof *P; i++) {
        struct ol_bezier b = {OL_FX(P[i][0]), OL_FX(P[i][1]), OL_FX(P[i][2]), OL_FX(P[i][3])};
        double worst = 0, worst_ol = 0;
        for (int t = 0; t <= 256; t++) {
            double r = webkit(P[i][0], P[i][1], P[i][2], P[i][3], t / 256.0) * 256;
            double o = ol_ease_bezier(P[i][0], P[i][1], P[i][2], P[i][3], t / 256.0) * 256;
            int v = ol_ease256_ex(OL_CUBIC_BEZIER, t, &b, 0);
            if (fabs(v - r) > worst) worst = fabs(v - r);
            if (fabs(v - o) > worst_ol) worst_ol = fabs(v - o);
        }
        printf("bezier %u max err %.3f (webkit) %.3f (double)\n", i, worst, worst_ol);
        CHECK(worst <= 1.0 && worst_ol <= 1.0, "bezier %u error %.3f/%.3f", i, worst, worst_ol);
        CHECK(ol_ease256_ex(OL_CUBIC_BEZIER, 0, &b, 0) == 0 && ol_ease256_ex(OL_CUBIC_BEZIER, 256, &b, 0) == 256,
              "bezier %u endpoints", i);
    }
    {
        struct ol_bezier css = {OL_FX(.25), OL_FX(.1), OL_FX(.25), OL_FX(1)};
        int mid = ol_ease256_ex(OL_CUBIC_BEZIER, 128, &css, 0);
        double w = webkit(.25, .1, .25, 1, .5) * 256;
        printf("css ease @0.5: %d vs webkit %.3f\n", mid, w);
        CHECK(fabs(mid - w) <= 1.0 && mid > 128, "bezier css ease midpoint %d", mid);
        CHECK(ol_ease256_ex(OL_CUBIC_BEZIER, 100, 0, 0) == 100, "bezier NULL is linear");
        struct ol_bezier bad = {OL_FX(1.5), 0, 0, OL_FX(1)};
        CHECK(ol_ease256_ex(OL_CUBIC_BEZIER, 100, &bad, 0) == 100, "bezier x out of range is linear");
    }
    /* steps: exact. */
    for (int n = 1; n <= 12; n++)
        for (int t = 0; t <= 256; t++) {
            int k = t >= 256 ? n : t * n / 256, ks = k + 1 > n ? n : k + 1;
            int e = t >= 256 ? 256 : (k * 256 + n / 2) / n;
            int s = (ks * 256 + n / 2) / n;
            CHECK(ol_ease256_ex(OL_STEPS, t, 0, n) == e, "steps end n=%d t=%d", n, t);
            CHECK(ol_ease256_ex(OL_STEPS, t, 0, -n) == s, "steps start n=%d t=%d", n, t);
        }
    CHECK(ol_ease256_ex(OL_STEPS, 100, 0, 0) == 100, "steps zero is linear");
    /* legacy vocabulary unchanged through the wrapper */
    for (int t = 0; t <= 256; t++)
        CHECK(ol_ease256(OL_EASE_IN, t) == t * t / 256 && ol_ease256(OL_LINEAR, t) == t, "legacy t=%d", t);
    CHECK(OL_EASE_OUT_CUBIC == 6 && OL_EASE_IN_CUBIC == 7, "enum values stable");
    /* unknown-curve counter is per context */
    gfx_ctx_hook = hook;
    CHECK(ol_ease_unknown_total() == 0, "counter starts at zero (%u)", ol_ease_unknown_total());
    for (int i = 0; i < 5; i++) {
        cur = 0;
        CHECK(ol_ease256(99, 77) == 77, "unknown is linear");
        cur = 1;
        if (i < 2) CHECK(ol_ease256(-1, 33) == 33, "unknown is linear");
    }
    CHECK(ol_ease_unknown[0] == 5 && ol_ease_unknown[1] == 2, "counter per ctx %u/%u",
          ol_ease_unknown[0], ol_ease_unknown[1]);
    CHECK(ol_ease_unknown_total() == 7, "counter total %u", ol_ease_unknown_total());
    printf(fails ? "%d FAILURES\n" : "ok\n", fails);
    return fails != 0;
}
