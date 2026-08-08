/* interp_test.c -- css_interp.c against WPT's own numbers.
 *
 * EVERY EXPECTED VALUE IN THIS FILE IS TRANSCRIBED, NOT DERIVED. The matrices
 * come out of css/css-transforms/transform-2d-getComputedStyle-001.html and
 * the interpolation triples out of
 * css/css-transforms/animation/transform-interpolation-001.html -- values
 * Chrome and Firefox actually produce, copied rather than worked out here.
 * That distinction has decided three lines tonight: a suite that derives its
 * expectations from the same reasoning as the implementation agrees with the
 * implementation and says nothing.
 *
 * The interpolation triples are compared the way WPT compares them: BOTH
 * sides through the same serialiser. WPT sets `transform: <expected>` on a
 * second element and compares the two elements' resolved values, so the test
 * here is
 *     serialize(matrix(interp(from, to, at)))  ==  serialize(matrix(parse(expected)))
 * which is the identical question and is why a `rotate3d(0.524083, ...)`
 * expectation with six digits of axis does not have to be reproduced to the
 * last digit -- it has to produce the same MATRIX, and the tolerance is
 * WPT's own 1e-5 relative.
 */
#include "css_interp.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

static int g_checks, g_fail;

static void ok(int cond, const char *what)
{
    g_checks++;
    if (!cond) { g_fail++; printf("  FAIL %s\n", what); }
}

static void eqs(const char *got, const char *want, const char *what)
{
    g_checks++;
    if (strcmp(got, want)) {
        g_fail++;
        printf("  FAIL %s\n       got  %s\n       want %s\n", what, got, want);
    }
}

/* WPT's own rounding step, verbatim in effect. transform-2d-getComputedStyle
 * -001.html reads the computed value and applies
 *     parseFloat(parseFloat(str).toFixed(6))
 * to every matrix argument before comparing, "to allow for small errors in
 * numerical precision" -- because rotate(90deg) genuinely computes
 * cos(pi/2) = 6.123e-17 and every engine, Chrome included, serialises that.
 * The expectations transcribed below are POST-rounding, so the rounding
 * belongs here too; making the engine emit a clean 0 instead would be
 * inventing a behaviour no browser has. */
static void round6(double m[16])
{
    for (int i = 0; i < 16; i++) {
        double v = m[i] * 1e6;
        v = (v < 0 ? -1.0 : 1.0) * floor(fabs(v) + 0.5);
        m[i] = v / 1e6;
        if (m[i] == 0.0) m[i] = 0.0;              /* normalise -0 */
    }
}

static void parse_m(const char *s, double m[16])
{
    struct ci_xform t;
    if (ci_transform_parse(s, -1, 16, 16, &t) != 0) {
        printf("  FAIL could not parse '%s'\n", s);
        g_fail++; g_checks++;
        for (int i = 0; i < 16; i++) m[i] = (i % 5) ? 0 : 1;
        return;
    }
    ci_transform_matrix(&t, 0, 0, m);
}

/* ---- 1. the resolved value: a transform list -> matrix(...) -------------- */
/* Transcribed from css/css-transforms/transform-2d-getComputedStyle-001.html */
static void test_serialize(void)
{
    static const struct { const char *css, *want; } V[] = {
        { "translate(10px, 20px)", "matrix(1, 0, 0, 1, 10, 20)" },
        { "translateX(10px)",      "matrix(1, 0, 0, 1, 10, 0)"  },
        { "translateY(20px)",      "matrix(1, 0, 0, 1, 0, 20)"  },
        { "rotate(90deg)",         "matrix(0, 1, -1, 0, 0, 0)"  },
        { "scale(2.0)",            "matrix(2, 0, 0, 2, 0, 0)"   },
        { "scaleX(0.5)",           "matrix(0.5, 0, 0, 1, 0, 0)" },
        { "scaleY(1.5)",           "matrix(1, 0, 0, 1.5, 0, 0)" },
        { "skewX(45deg)",          "matrix(1, 0, 1, 1, 0, 0)"   },
        { "skewY(45deg)",          "matrix(1, 1, 0, 1, 0, 0)"   },
        { "matrix(1, 2, 3, 4, 5, 6)", "matrix(1, 2, 3, 4, 5, 6)" },
        { "none",                  "matrix(1, 0, 0, 1, 0, 0)"   },
    };
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++) {
        double m[16];
        char b[512];
        parse_m(V[i].css, m);
        round6(m);
        ci_matrix_text(m, b, sizeof b);
        eqs(b, V[i].want, V[i].css);
    }

    /* THE ORDER OF MULTIPLICATION, which is the one thing a single-function
     * test cannot check. Chrome: translate(100px) rotate(90deg) is
     * matrix(0, 1, -1, 0, 100, 0) -- the translate stays whole, because the
     * list composes left-to-right with the first function outermost. Reverse
     * the product and the translate comes out rotated, as (0, 100). */
    double m[16]; char b[512];
    parse_m("translate(100px) rotate(90deg)", m);
    round6(m);
    ci_matrix_text(m, b, sizeof b);
    eqs(b, "matrix(0, 1, -1, 0, 100, 0)", "list order: translate then rotate");

    parse_m("rotate(90deg) translate(100px)", m);
    round6(m);
    ci_matrix_text(m, b, sizeof b);
    eqs(b, "matrix(0, 1, -1, 0, 0, 100)", "list order: rotate then translate");

    /* A 3D function must serialise as matrix3d, not silently flatten. */
    parse_m("translateZ(10px)", m);
    round6(m);
    ci_matrix_text(m, b, sizeof b);
    eqs(b, "matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 10, 1)", "translateZ is 3D");
}

/* ---- 2. the computed value: the function list, lengths absolute ---------- */
static void test_computed_text(void)
{
    static const struct { const char *css, *want; } V[] = {
        { "translate(10px, 20px)", "translate(10px, 20px)" },
        { "translate(10px)",       "translate(10px)"       },
        { "translate(1em, 2em)",   "translate(16px, 32px)" },   /* fs = 16 */
        { "translateX(50%)",       "translateX(50%)"       },
        { "rotate(0.25turn)",      "rotate(90deg)"         },
        { "scale(2)",              "scale(2)"              },
        { "scale(2, 3)",           "scale(2, 3)"           },
        { "perspective(none)",     "perspective(none)"     },
        { "perspective(400px)",    "perspective(400px)"    },
        { "none",                  "none"                  },
    };
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++) {
        struct ci_xform t;
        char b[512];
        int r = ci_transform_parse(V[i].css, -1, 16, 16, &t);
        ok(r == 0, V[i].css);
        ci_transform_text(&t, b, sizeof b);
        eqs(b, V[i].want, V[i].css);
    }
}

/* ---- 3. what must NOT parse -------------------------------------------- */
static void test_invalid(void)
{
    static const char *const BAD[] = {
        "translate(10px 20px)",        /* whitespace instead of a comma */
        "translate()",
        "rotate(10)",                  /* a bare number is not an angle... */
        "scale(2px)",                  /* ...and a length is not a number */
        "translateZ(50%)",             /* no z reference length exists */
        "banana(1)",
        "matrix(1, 2, 3)",
        "rotate(30deg",
        0
    };
    for (int i = 0; BAD[i]; i++) {
        struct ci_xform t;
        char w[128];
        snprintf(w, sizeof w, "rejects '%s'", BAD[i]);
        ok(ci_transform_parse(BAD[i], -1, 16, 16, &t) != 0, w);
    }
    /* rotate(10) is invalid; rotate(0) is not -- a unitless zero is a valid
     * <angle> nowhere, but it IS a valid length, so this only checks the
     * angle rule stays on the angle. */
    struct ci_xform t;
    ok(ci_transform_parse("translate(0)", -1, 16, 16, &t) == 0, "translate(0) is valid");
}

/* ---- 4. interpolation, WPT's own triples -------------------------------- */
/* `tol` is 0 for "WPT's own 1e-5 relative". It is loosened only where the
 * TRANSCRIPTION is the limit rather than the engine: the two quaternion-slerp
 * rows below quote their result as `rotate3d(0.524083, 0.804261, 0.280178,
 * 106.91deg)` -- an axis to six digits but an angle to two decimals, which is
 * 5e-5 of relative slack before any arithmetic happens. Holding those to 1e-5
 * would be holding the engine to a precision the expected value does not
 * carry. Every other row keeps the tight bound. */
struct trip { const char *from, *to; double at; const char *expect; double tol; };

/* transform-interpolation-001.html, transcribed. */
static const struct trip TRIPS[] = {
    /* rotate: matched lists, angle interpolation, past both ends */
    { "rotate(30deg)",  "rotate(330deg)", -1,   "rotate(-270deg)", 0 },
    { "rotate(30deg)",  "rotate(330deg)",  0,   "rotate(30deg)",   0 },
    { "rotate(30deg)",  "rotate(330deg)",  0.25,"rotate(105deg)",  0 },
    { "rotate(30deg)",  "rotate(330deg)",  0.75,"rotate(255deg)",  0 },
    { "rotate(30deg)",  "rotate(330deg)",  1,   "rotate(330deg)",  0 },
    { "rotate(30deg)",  "rotate(330deg)",  2,   "rotate(630deg)",  0 },
    { "rotateX(0deg)",  "rotateX(700deg)", 0.25,"rotateX(175deg)", 0 },
    { "rotateY(0deg)",  "rotateY(800deg)", 0.75,"rotateY(600deg)", 0 },
    { "rotateZ(0deg)",  "rotateZ(900deg)", 0.25,"rotateZ(225deg)", 0 },

    /* a common axis when EITHER endpoint's angle is zero -- the axis of the
     * zero rotation carries no information and is taken from the other end */
    { "rotateX(0deg)",  "rotateY(900deg)", 0.25,"rotateY(225deg)", 0 },
    { "rotateX(0deg)",  "rotateY(900deg)", -1,  "rotateY(-900deg)", 0 },
    { "rotateY(900deg)","rotateZ(0deg)",   0.75,"rotateY(225deg)", 0 },

    /* rotate3d: same axis */
    { "rotate3d(7, 8, 9, 100deg)", "rotate3d(7, 8, 9, 260deg)", 0.25, "rotate3d(7, 8, 9, 140deg)", 0 },
    { "rotate3d(7, 8, 9, 100deg)", "rotate3d(7, 8, 9, 260deg)", -1,   "rotate3d(7, 8, 9, -60deg)", 0 },
    /* colinear but not equal axes count as the same axis */
    { "rotate3d(0, 1, 0, 0deg)",   "rotate3d(0, 2, 0, 450deg)", 0.25, "rotate3d(0, 1, 0, 112.5deg)", 0 },
    /* NOT colinear: quaternion slerp */
    { "rotate3d(1, 1, 0, 90deg)",  "rotate3d(0, 1, 1, 180deg)", 0.25,
      "rotate3d(0.524083, 0.804261, 0.280178, 106.91deg)", 3e-3 },
    { "rotate3d(1, 1, 0, 90deg)",  "rotate3d(0, 1, 1, 180deg)", 0.75,
      "rotate3d(0.163027, 0.774382, 0.611354, 153.99deg)", 3e-3 },

    /* `none` is the identity of whatever the other side is */
    { "none",          "rotate(90deg)", 0.25, "rotate(22.5deg)", 0 },
    { "rotate(90deg)", "none",          0.25, "rotate(67.5deg)", 0 },
    { "none",          "rotate(90deg)", 2,    "rotate(180deg)",  0 },

    /* a matched multi-function list interpolates item by item */
    { "rotateX(0deg) rotateY(0deg) rotateZ(0deg)",
      "rotateX(700deg) rotateY(800deg) rotateZ(900deg)", 0.25,
      "rotateX(175deg) rotateY(200deg) rotateZ(225deg)", 0 },

    /* perspective interpolates its RECIPROCAL. WPT computes these with
     *   1 / ((1 - p) / from + p / to)
     * and the two below are that helper evaluated at 0.25 and 0.75 for
     * (400, 500):  1/(0.75/400 + 0.25/500) = 421.052631578..
     *              1/(0.25/400 + 0.75/500) = 470.588235294.. */
    { "perspective(400px)", "perspective(500px)", 0.25, "perspective(421.0526315789474px)", 0 },
    { "perspective(400px)", "perspective(500px)", 0.75, "perspective(470.5882352941176px)", 0 },
    /* perspective(none) is the identity: 1/inf == 0 */
    { "perspective(none)",  "perspective(500px)", 0.5,  "perspective(1000px)", 0 },
    { "perspective(none)",  "perspective(500px)", 2,    "perspective(250px)",  0 },
    { "scaleZ(2)", "scaleZ(2) perspective(500px)", 0.5, "scaleZ(2) perspective(1000px)", 0 },

    /* skew and a trailing perspective, both matched */
    { "skewX(10rad) perspective(400px)", "skewX(20rad) perspective(500px)", 0.25,
      "skewX(12.5rad) perspective(421.0526315789474px)", 0 },

    /* translate/scale families reduce to a common primitive */
    { "translateX(0px)", "translate3d(100px, 200px, 300px)", 0.5,
      "translate3d(50px, 100px, 150px)", 0 },
    { "scaleX(1)",       "scale3d(3, 5, 7)", 0.5, "scale3d(2, 3, 4)", 0 },
    { "scale(2)",        "scaleY(4)",        0.5, "scale(1.5, 3)",    0 },
};

static int matrices_tol(const double a[16], const double b[16], double tol)
{
    for (int i = 0; i < 16; i++) {
        double d = fabs(a[i] - b[i]);
        if (d <= tol) continue;
        double s = fabs(a[i]) < fabs(b[i]) ? fabs(a[i]) : fabs(b[i]);
        if (s < 1e-6) s = 1e-6;
        if (d / s > 1e-5 && d > 1e-9) return 0;
    }
    return 1;
}

static int matrices_close(const double a[16], const double b[16])
{ return matrices_tol(a, b, 0.0); }

static void test_interp_trips(void)
{
    for (unsigned i = 0; i < sizeof TRIPS / sizeof TRIPS[0]; i++) {
        const struct trip *t = &TRIPS[i];
        struct ci_xform a, b, r, e;
        char what[320];
        snprintf(what, sizeof what, "%s -> %s at %g == %s",
                 t->from, t->to, t->at, t->expect);
        if (ci_transform_parse(t->from, -1, 16, 16, &a) ||
            ci_transform_parse(t->to,   -1, 16, 16, &b) ||
            ci_transform_parse(t->expect, -1, 16, 16, &e)) {
            g_checks++; g_fail++;
            printf("  FAIL parse: %s\n", what);
            continue;
        }
        ci_transform_interp(&a, &b, t->at, &r);
        double mr[16], me[16];
        ci_transform_matrix(&r, 0, 0, mr);
        ci_transform_matrix(&e, 0, 0, me);
        g_checks++;
        if (!matrices_tol(mr, me, t->tol)) {
            g_fail++;
            char g[512], w[512];
            ci_matrix_text(mr, g, sizeof g);
            ci_matrix_text(me, w, sizeof w);
            printf("  FAIL %s\n       got  %s\n       want %s\n", what, g, w);
        }
    }
}

/* ---- 5. the decomposition path ----------------------------------------- */
/* THE ASSERTION THE NEGATIVE CONTROL EXISTS FOR. A mismatched pair --
 * rotate against scale -- has no common primitive, so it MUST go through
 * decomposition; and the value at the halfway point must be the recomposed
 * matrix, not a componentwise blend of a rotate and a scale.
 *
 * Both halves are checked, because either alone can pass while the other
 * fails: the counter says the decomposition ran, the value says it ran
 * correctly. Componentwise-always makes the counter zero AND the value
 * wrong; a broken decomposition makes only the value wrong. */
static void test_decompose_path(void)
{
    struct ci_xform a, b, r;
    ci_transform_parse("rotate(90deg)", -1, 16, 16, &a);
    ci_transform_parse("scale(2)", -1, 16, 16, &b);

    ci_decompose_count_reset();
    ci_transform_interp(&a, &b, 0.5, &r);
    ok(ci_decompose_count() == 1,
       "rotate vs scale takes the decomposition path (counter)");

    /* At p = 0.5 the decomposition is rotate 45deg, scale 1.5 -- and the
     * recomposed matrix is R(45) * S(1.5), which is
     *   [1.5cos45, 1.5sin45, -1.5sin45, 1.5cos45] = [1.06066, 1.06066, ...]
     * Componentwise-always would pair rotate's 90deg against scale's 2 and
     * produce a smooth, finite, completely different matrix. */
    double m[16];
    char got[512];
    ci_transform_matrix(&r, 0, 0, m);
    ci_matrix_text(m, got, sizeof got);
    eqs(got, "matrix(1.06066, 1.06066, -1.06066, 1.06066, 0, 0)",
        "rotate(90deg) vs scale(2) at 0.5 is the recomposed matrix");

    /* A matched pair must NOT decompose -- decomposing everything is the
     * opposite error and loses the function list. */
    ci_decompose_count_reset();
    struct ci_xform c, d, r2;
    ci_transform_parse("rotate(0deg)", -1, 16, 16, &c);
    ci_transform_parse("rotate(90deg)", -1, 16, 16, &d);
    ci_transform_interp(&c, &d, 0.5, &r2);
    ok(ci_decompose_count() == 0, "a matched pair does not decompose");
}

/* ---- 6. decompose/recompose round-trip ---------------------------------- */
/* The property that makes the whole path trustworthy: for any matrix that is
 * not singular, recompose(decompose(m)) == m. It catches the convention bugs
 * this file is full of opportunities for -- a transposed quaternion, the skew
 * shears applied as three matrices instead of one, the perspective row solved
 * the wrong way round -- none of which show up in an interpolation test
 * whose endpoints happen to be symmetric. */
static void test_roundtrip(void)
{
    static const char *const CASES[] = {
        "rotate(37deg)",
        "scale(2, 3)",
        "translate(10px, 20px) rotate(30deg) scale(1.5)",
        "skewX(20deg)",
        "skew(20deg, 10deg)",
        "rotate3d(1, 2, 3, 40deg)",
        "translate3d(1px, 2px, 3px) rotate3d(3, 2, 1, 70deg) scale3d(1.5, 2, 2.5)",
        "perspective(400px) rotateY(30deg)",
        "matrix(1, 2, 3, 4, 5, 6)",
        "rotateX(20deg) skewY(15deg) scaleZ(3)",
        0
    };
    for (int i = 0; CASES[i]; i++) {
        double m[16], back[16];
        struct ci_decomp d;
        char w[160];
        parse_m(CASES[i], m);
        snprintf(w, sizeof w, "round-trip %s", CASES[i]);
        if (!ci_decompose(m, &d)) { ok(0, w); continue; }
        ci_recompose(&d, back);
        ok(matrices_close(back, m), w);
        if (!matrices_close(back, m)) {
            char g[512], e[512];
            ci_matrix_text(back, g, sizeof g);
            ci_matrix_text(m, e, sizeof e);
            printf("       got  %s\n       want %s\n", g, e);
        }
    }
}

/* ---- 7. number serialisation ------------------------------------------- */
static void test_numbers(void)
{
    static const struct { double v; const char *want; } N[] = {
        { 0.0, "0" }, { -0.0, "0" }, { 1.0, "1" }, { 2.0, "2" },
        { 0.5, "0.5" }, { 1.5, "1.5" }, { -1.0, "-1" },
        { 100.0, "100" }, { 1000000.0, "1000000" },
        { 0.8660254037844387, "0.866025" },
        { 1.0 / 3.0, "0.333333" },
        { 123456789.0, "123457000" },
    };
    for (unsigned i = 0; i < sizeof N / sizeof N[0]; i++) {
        char b[64];
        ci_num_text(N[i].v, b, sizeof b);
        eqs(b, N[i].want, "number");
    }
}

/* ---- 8. the generic path ------------------------------------------------ */
static void test_generic(void)
{
    static const struct { const char *p, *a, *b; double at; const char *want; } G[] = {
        { "margin-left", "0px",  "20px",  0.3,  "6px" },
        { "margin-left", "10px", "20px",  0.5,  "15px" },
        { "margin-left", "0px",  "20px", -0.3,  "-6px" },
        { "margin-left", "0px",  "20px",  1.5,  "30px" },
        { "opacity",     "0",    "1",     0.25, "0.25" },
        { "color",       "rgb(0, 0, 0)", "rgb(100, 200, 40)", 0.5, "rgb(50, 100, 20)" },
        { "background-position", "0px 0px", "100px 200px", 0.5, "50px 100px" },
        { "border-left", "2px solid red", "10px solid red", 0.5, "6px solid red" },
    };
    for (unsigned i = 0; i < sizeof G / sizeof G[0]; i++) {
        char b[256], w[256];
        int r = ci_value_interp(G[i].p, G[i].a, G[i].b, G[i].at, b, sizeof b);
        snprintf(w, sizeof w, "%s: %s -> %s at %g", G[i].p, G[i].a, G[i].b, G[i].at);
        ok(r >= 0, w);
        if (r >= 0) eqs(b, G[i].want, w);
    }

    /* Declines rather than guesses. Each of these has a shape mismatch that
     * no componentwise rule can bridge, and answering them with a blend is
     * worse than answering with the discrete flip the caller will apply. */
    char b[256];
    ok(ci_value_interp("width", "10px", "auto", 0.5, b, sizeof b) < 0,
       "declines a length against a keyword");
    ok(ci_value_interp("display", "block", "inline", 0.5, b, sizeof b) < 0,
       "declines a discrete property");
    ok(ci_value_interp("transform", "rotate(0deg)", "rotate(90deg)", 0.5, b, sizeof b) < 0,
       "declines transform -- it has its own API");
    ok(ci_prop_is_discrete("visibility"), "visibility is discrete");
    ok(!ci_prop_is_discrete("opacity"), "opacity is not discrete");
}

int main(void)
{
    printf("css_interp: transform values and CSS value interpolation\n");
    test_numbers();
    test_serialize();
    test_computed_text();
    test_invalid();
    test_interp_trips();
    test_decompose_path();
    test_roundtrip();
    test_generic();
    printf("css_interp: %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { printf("FAIL\n"); return 1; }
    printf("ok\n");
    return 0;
}
