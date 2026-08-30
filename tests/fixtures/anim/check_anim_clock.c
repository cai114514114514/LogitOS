/* check_anim_clock.c -- the host half of the anim gate (tests/anim.mk).
 *
 * WHAT IT MEASURES. css_interp.c's timing functions (ci_ease_parse /
 * ci_ease_apply) against reference values, and js_anim.c's animation/
 * transition shorthand parsers (css_anim_parse_animation /
 * css_anim_parse_transition). No DOM, no clock: these are the pure halves
 * of the animation engine, exactly the parts a host link can drive without
 * a guest -- the page-level proof lives in tests/qmp/qmp_anim_page.py.
 *
 * WHY MID-CURVE. Both endpoints of every timing function are pinned by
 * construction (t<=0 -> 0, t>=1 -> 1, returned early in ci_ease_apply), so
 * a suite that only checks endpoints cannot fail. Every value asserted
 * below is strictly inside (0,1) -- that is the entire point of this file,
 * and it is why the negative control (-DCSS_ANIM_NEGCTL_EASE_LINEAR, which
 * collapses every easing to identity while leaving the endpoints alone)
 * MUST redden it: identity is wrong by >= 0.03 at every point chosen here
 * (smallest margin: steps(4) at 0.3, |0.3-0.25| = 0.05) and the tolerance
 * is 1e-3 for beziers, exact for steps.
 *
 * WHERE THE NUMBERS COME FROM, so nobody has to trust them:
 *   - the four named beziers are cubic-bezier(0.25,0.1,0.25,1),
 *     (0.42,0,1,1), (0,0,0.58,1), (0.42,0,0.58,1) per CSS Easing 1;
 *   - reference values were solved on the host in Python by bisection
 *     over x(t) to full double precision (200 iterations, |dx| < 1e-15),
 *     i.e. the same math the implementation does but with no iteration
 *     budget -- the implementation's Newton+bisection hybrid claims 1e-7
 *     and the 1e-3 tolerance leaves three orders of margin for it;
 *   - steps() references are the spec's own algorithm (css-easing-1
 *     2.3.1: current step = floor(x*n), +1 for jump-start/jump-both,
 *     jumps = n / n-1 / n+1, y = current/jumps), evaluated by hand at
 *     the asserted points -- dyadic rationals, asserted exactly.
 *
 * THE STEPS ROWS ARE NOT DECORATION. The first run of this checker
 * against the recovered half-edit reddened jump-end (the code returned
 * (floor+1)/n where the spec says floor/n -- 0.5 where 0.25 belongs),
 * jump-none ((floor+1)/(n+1) where the spec says floor(x*n)/(n-1)), and
 * two parser accepts the spec forbids (steps(1, jump-none) is invalid
 * because jump-none needs n>1; steps(2.5) is invalid because the count is
 * an <integer>). jump-start and jump-both were already right. If those
 * rows ever go green-by-accident, re-derive them; do not delete them.
 *
 * OUTPUT CONTRACT. One line per assertion: "ok   <label> ..." or
 * "FAIL <label> ...". All assertions run (no early exit) so the negative
 * control's grep ('FAIL ease-in-out(0.25)') sees the line even when
 * earlier rows also fail. Exit 0 iff nothing failed. tests/anim.mk builds
 * this twice: as-is, and with -DCSS_ANIM_NEGCTL_EASE_LINEAR where it must
 * FAIL -- a checker that passes in both worlds is measuring nothing.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "css.h"          /* ci_ease_*, struct anim_spec/trans_spec, parsers */

static int g_fail;
static int g_run;

static void ck_close(const char *label, double got, double exp, double tol)
{
    g_run++;
    if (fabs(got - exp) <= tol) {
        printf("ok   %s -> %.9f (expected %.9f)\n", label, got, exp);
    } else {
        g_fail++;
        printf("FAIL %s -> %.9f expected %.9f (tol %g)\n", label, got, exp, tol);
    }
}

static void ck_int(const char *label, long got, long exp)
{
    g_run++;
    if (got == exp) {
        printf("ok   %s -> %ld\n", label, got);
    } else {
        g_fail++;
        printf("FAIL %s -> %ld expected %ld\n", label, got, exp);
    }
}

/* Parse s as a timing function and ease one point through it.
 * kind==0 asserts the PARSE fails (and never calls apply). */
static void ease_pt(const char *s, double t, double exp, int parse_must_fail)
{
    struct ci_ease e;
    char label[96];
    if (parse_must_fail) {
        snprintf(label, sizeof label, "parse rejects %s", s);
        ck_int(label, ci_ease_parse(s, (int)strlen(s), &e), -1);
        return;
    }
    if (ci_ease_parse(s, (int)strlen(s), &e) != 0) {
        g_run++; g_fail++;
        printf("FAIL %s(%g) -- ci_ease_parse refused the value\n", s, t);
        return;
    }
    snprintf(label, sizeof label, "%s(%g)", s, t);
    ck_close(label, ci_ease_apply(&e, t), exp, 1e-3);
}

/* ---- the animation shorthand ------------------------------------------ */

static struct anim_spec as_of(const char *v, int want_ok, const char *label)
{
    struct anim_spec a;
    int r = css_anim_parse_animation(v, (int)strlen(v), &a);
    if (want_ok) {
        ck_int(label, r, 0);
        if (r != 0) { memset(&a, 0, sizeof a); return a; }
    } else {
        ck_int(label, r, -1);
        memset(&a, 0, sizeof a);
    }
    return a;
}

static void anim_shorthand_checks(void)
{
    struct anim_spec a;

    a = as_of("fade 2s linear", 1, "anim: 'fade 2s linear' parses");
    ck_int("anim: ...name is fade", strcmp(a.name, "fade") == 0 && a.has_name, 1);
    ck_close("anim: ...duration 2000ms", a.dur_ms, 2000.0, 1e-9);
    ck_int("anim: ...ease is linear", a.ease.kind, CI_EASE_LINEAR);

    a = as_of("slide 350ms ease-in-out .5s 3 alternate forwards paused", 1,
              "anim: the full keyword soup parses");
    ck_close("anim: ...dur 350, delay 500 (first two times, any order)",
             a.dur_ms * 1000 + a.delay_ms, 350000 + 500.0, 1e-6);
    ck_close("anim: ...iteration count 3", a.iters, 3.0, 1e-9);
    ck_int("anim: ...direction alternate", a.dir, CAD_ALTERNATE);
    ck_int("anim: ...fill forwards", a.fill_fwd, 1);
    ck_int("anim: ...paused", a.paused, 1);
    ck_int("anim: ...ease is a cubic", a.ease.kind, CI_EASE_CUBIC);
    ck_close("anim: ...ease-in-out x1=.42", a.ease.x1, 0.42, 1e-9);

    a = as_of("2s linear fade", 1, "anim: position-independent grammar");
    ck_int("anim: ...name still fade", strcmp(a.name, "fade") == 0, 1);
    ck_close("anim: ...duration 2000ms", a.dur_ms, 2000.0, 1e-9);

    a = as_of("spin 1s cubic-bezier(.4,0,.6,1) infinite reverse", 1,
              "anim: cubic-bezier is ONE token (parens shelter the commas)");
    ck_int("anim: ...infinite", a.infinite, 1);
    ck_int("anim: ...direction reverse", a.dir, CAD_REVERSE);
    ck_close("anim: ...bezier x1=.4", a.ease.x1, 0.4, 1e-9);

    a = as_of("fade", 1, "anim: bare name parses (dur 0: engine declines)");
    ck_int("anim: ...has_name", a.has_name, 1);
    ck_close("anim: ...dur_ms 0", a.dur_ms, 0.0, 1e-9);

    as_of("foo bar", 0, "anim: two bare idents are ambiguous -> refuse");
    as_of("fade 1s 2s 3s", 0, "anim: three times -> refuse");

    /* The CSS initial: no iteration count spelled -> 1, not 0. */
    a = as_of("fade 1s", 1, "anim: iters default");
    ck_close("anim: ...iters 1", a.iters, 1.0, 1e-9);

    /* A SPELLED zero is legal (runs zero iterations); the parser must
     * hand it through honestly rather than "fix" it to 1 -- anim_progress
     * treats iter >= iters as finished, so 0 means "no animation", which
     * is what the author asked for. */
    a = as_of("fade 1s 0", 1, "anim: spelled zero iteration count parses");
    ck_close("anim: ...iters stays 0", a.iters, 0.0, 1e-9);

    /* Negative counts are not <number> per the animations spec. */
    as_of("fade 1s -2", 0, "anim: negative iteration count -> refuse");
}

/* ---- the transition shorthand ----------------------------------------- */

/* Thin wrappers so no call site ever hand-counts a length again: the
 * first red run of this file reddened two rows because I did ("opacity
 * 2s linear" passed as 19, is 17 -- the parser then read past the value
 * into stack garbage and refused). The engine always passes the span's
 * true length; the checker must too, or it is testing a different parser
 * input than the one that ships. */
static int trans_try(const char *v, const char *prop, struct trans_spec *out)
{
    return css_anim_parse_transition(v, (int)strlen(v), prop, out);
}

static void trans_checks(void)
{
    struct trans_spec t;

    ck_int("trans: 'opacity 2s linear' covers opacity",
           trans_try("opacity 2s linear", "opacity", &t), 0);
    ck_close("trans: ...dur 2000ms", t.dur_ms, 2000.0, 1e-9);
    ck_int("trans: ...ease linear", t.ease.kind, CI_EASE_LINEAR);
    ck_int("trans: ...does NOT cover transform",
           trans_try("opacity 2s linear", "transform", &t), -1);

    ck_int("trans: 'all .3s ease-out' covers opacity",
           trans_try("all .3s ease-out", "opacity", &t), 0);
    ck_close("trans: ...dur 300ms", t.dur_ms, 300.0, 1e-9);
    ck_int("trans: 'all .3s ease-out' covers transform too",
           trans_try("all .3s ease-out", "transform", &t), 0);

    ck_int("trans: comma list finds the named item (transform)",
           trans_try("transform .4s, opacity 1s .2s", "transform", &t), 0);
    ck_close("trans: ...transform dur 400ms", t.dur_ms, 400.0, 1e-9);
    ck_int("trans: comma list finds the other item (opacity)",
           trans_try("transform .4s, opacity 1s .2s", "opacity", &t), 0);
    ck_close("trans: ...opacity dur 1000 delay 200",
             t.dur_ms * 1000 + t.delay_ms, 1000000.0 + 200.0, 1e-6);

    ck_int("trans: 'none' covers nothing",
           trans_try("none", "opacity", &t), -1);
    ck_int("trans: an unrelated property is not covered",
           trans_try("width .2s", "opacity", &t), -1);

    /* Duration 0 is the CSS initial and parses; the ENGINE declines to
     * start a 0 ms transition, so the value reaching it must be honest. */
    ck_int("trans: 'opacity' alone parses",
           trans_try("opacity", "opacity", &t), 0);
    ck_close("trans: ...dur_ms 0", t.dur_ms, 0.0, 1e-9);
}

int main(void)
{
    /* ---- linear: parse + apply, and the endpoints pinned by construction */
    {
        struct ci_ease e;
        ck_int("linear parses", ci_ease_parse("linear", 6, &e), 0);
        ck_int("linear is CI_EASE_LINEAR", e.kind, CI_EASE_LINEAR);
        ease_pt("linear", 0.25, 0.25, 0);
        /* Endpoints: pinned for EVERY easing, checked once here. */
        ck_close("linear(0) pinned to 0", ci_ease_apply(&e, 0.0), 0.0, 0.0);
        ck_close("linear(1) pinned to 1", ci_ease_apply(&e, 1.0), 1.0, 0.0);
    }

    /* ---- the four named beziers (CSS Easing 1's constants) --------------
     * References: bisection over x(t) to double precision (see header). */
    ease_pt("ease",        0.25, 0.408510591, 0);
    ease_pt("ease",        0.75, 0.960458978, 0);
    ease_pt("ease-in",     0.25, 0.093464651, 0);
    ease_pt("ease-out",    0.25, 0.378138131, 0);
    ease_pt("ease-out",    0.75, 0.906535349, 0);
    /* The label the negative control greps for: it must stay EXACTLY
     * 'ease-in-out(0.25)'. Identity gives 0.25, the real curve 0.129 --
     * a 0.12 gap that a 1e-3 tolerance cannot mistake. */
    ease_pt("ease-in-out", 0.25, 0.129161931, 0);
    ease_pt("ease-in-out", 0.75, 0.870838069, 0);

    /* ---- cubic-bezier(): arbitrary control points ---------------------- */
    ease_pt("cubic-bezier(.4,0,.6,1)",   0.3, 0.194157469, 0);
    ease_pt("cubic-bezier(.1,.9,.2,1)",  0.2, 0.794393691, 0);
    /* y outside [0,1] is legal CSS; ci_ease_apply CLAMPS its result (a
     * documented contract in css.h: opacity cannot overshoot). Both ends
     * of the overshoot, so neither direction is untested. */
    ease_pt("cubic-bezier(.68,-0.55,.27,1.55)", 0.2, 0.0, 0);
    ease_pt("cubic-bezier(.68,-0.55,.27,1.55)", 0.75, 1.0, 0);
    /* x outside [0,1] is NOT legal (the curve stops being a function) and
     * must be refused, never clamped into a different curve. */
    ease_pt("cubic-bezier(1.5,0,.5,1)",  0.25, 0.0, 1);
    ease_pt("cubic-bezier(.4,0)",        0.25, 0.0, 1);

    /* ---- steps(): the spec algorithm, hand-evaluated --------------------
     * jump-end   y = floor(x*n)/n        steps(4)(0.3) = 1/4
     * jump-start y = (floor(x*n)+1)/n    steps(4, jump-start)(0.3) = 2/4
     * jump-none  y = floor(x*n)/(n-1)    steps(2, jump-none)(0.3) = 0/1
     * jump-both  y = (floor(x*n)+1)/(n+1) steps(2, jump-both)(0.3) = 1/3
     * Points are kept OFF step boundaries (t*n integral) -- the boundary
     * is the before-flag corner the clock never samples at 20 ms. */
    ease_pt("steps(4)",             0.3, 0.25, 0);
    ease_pt("steps(4, end)",        0.3, 0.25, 0);
    ease_pt("steps(4, jump-end)",   0.26, 0.25, 0);
    ease_pt("steps(4, jump-start)", 0.3, 0.5, 0);
    ease_pt("steps(4, start)",      0.3, 0.5, 0);
    ease_pt("steps(2, jump-none)",  0.3, 0.0, 0);
    ease_pt("steps(2, jump-none)",  0.6, 1.0, 0);
    ease_pt("steps(4, jump-none)",  0.3, 1.0 / 3.0, 0);
    ease_pt("steps(2, jump-both)",  0.3, 1.0 / 3.0, 0);
    ease_pt("steps(4, jump-both)",  0.7, 0.6, 0);
    ease_pt("step-start",           0.25, 1.0, 0);
    ease_pt("step-end",             0.25, 0.0, 0);
    ease_pt("step-end",             0.9, 0.0, 0);
    /* steps(1, jump-none): n must be >1 when the position is jump-none
     * (otherwise jumps = n-1 = 0 and y has a zero denominator) -- the
     * spec makes it invalid SYNTAX, so the parser refuses it. steps(2.5):
     * the count is an <integer>. steps(0): a positive integer. */
    ease_pt("steps(1, jump-none)",  0.3, 0.0, 1);
    ease_pt("steps(2.5)",           0.3, 0.0, 1);
    ease_pt("steps(0)",             0.3, 0.0, 1);
    ease_pt("steps(3, wobble)",     0.3, 0.0, 1);
    ease_pt("banana",               0.25, 0.0, 1);

    anim_shorthand_checks();
    trans_checks();

    printf("\ncheck_anim_clock: %d checks, %d failures\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
