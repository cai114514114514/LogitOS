/* The rim refraction table, checked against double precision.
 *
 * WHAT IS AND IS NOT BEING VERIFIED, because it would be easy to overclaim.
 * c/lib/gfx is checked against an independent oracle: coverage has an analytic
 * truth that owes nothing to the rasterizer. A refraction curve has no such
 * oracle -- its truth IS the closed form in glass.c, and evaluating that form
 * to check itself is a tautology. So this test verifies the ONE thing that can
 * actually be wrong here: the fixed-point arithmetic. Two square roots, a
 * 16.16 division and a normalisation, every one of them a place to lose a
 * factor of 65536 or overflow a long long. The reference below is the same
 * formula in double, and the assertion is that the integer table agrees with
 * it to within one pixel, everywhere, over every geometry the panel can ask
 * for. That is a claim about the port, not about the optics.
 *
 * The structural properties ARE independent of the formula, and are asserted
 * separately: the displacement never increases inward, blue always bends at
 * least as much as red, the outermost pixel is exactly the caller's REFRACT,
 * and nothing past the edge band is nonzero.
 *
 *   cc -o glass_lut_test tests/unit/glass_lut_test.c c/kernel/gui/glass.c -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "glass.h"

static int fails, checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("  FAIL: %s\n", what); }
}

/* a/(h+d) at incidence parameter u, refractive index eta -- the same expression
 * glass_refract_ratio() implements, in double. */
static double ref_ratio(double u, double eta)
{
    if (u <= 0.0) return 0.0;
    double ca = sqrt(1.0 - u * u);
    double se = u / eta;
    double cb = sqrt(1.0 - se * se);
    double num = u * cb - ca * se;
    double den = ca * cb + u * se;
    if (den <= 0.0) return 0.0;
    return num / den;
}

int main(void)
{
    /* Must match glass.c. Duplicated on purpose: if someone retunes the fan,
     * this test fails until they retune it here too, which is the point. */
    static const int en[3] = { 144, 150, 156 };
    static const char *cn[3] = { "R", "G", "B" };

    double worst = 0.0;
    int worst_e = 0, worst_r = 0, worst_c = 0, worst_s = 0;

    printf("glass refraction LUT: integer vs double\n");

    for (int E = 1; E <= GLASS_E_MAX; E++) {
        for (int refract = 1; refract <= E; refract++) {
            glass_build_lut(E, refract);

            /* Green's peak normalises all three -- see glass.c. Using each
             * channel's own peak here would make the test agree with the bug
             * it exists to catch. */
            double peak = ref_ratio(1.0, en[1] / 100.0);

            for (int c = 0; c < 3; c++) {
                double eta = en[c] / 100.0;
                int prev = -1;

                for (int s = 0; s <= E; s++) {
                    double u = (double)(E - s) / E;
                    double want = ref_ratio(u, eta) * refract / peak;
                    int got = glass_disp[c][s];
                    double err = fabs(got - want);

                    if (err > worst) {
                        worst = err;
                        worst_e = E; worst_r = refract; worst_c = c; worst_s = s;
                    }
                    if (err > 0.51) {
                        char m[128];
                        snprintf(m, sizeof m,
                                 "E=%d refract=%d %s s=%d: got %d, double says %.4f",
                                 E, refract, cn[c], s, got, want);
                        ck(0, m);
                    } else {
                        checks++;
                    }

                    /* Monotone inward: the bend can only weaken as the normal
                     * flattens. A sign or shift error shows up here first. */
                    if (prev >= 0 && got > prev) {
                        char m[128];
                        snprintf(m, sizeof m, "E=%d refract=%d %s: disp rises inward at s=%d (%d > %d)",
                                 E, refract, cn[c], s, got, prev);
                        ck(0, m);
                    } else checks++;
                    prev = got;
                }

                /* GREEN's outermost pixel is the normalisation point: the
                 * panel's REFRACT still means exactly what it meant before the
                 * curve changed, which is why nothing had to be retuned. Red
                 * and blue must NOT equal it -- that was the bug. */
                if (c == 1) {
                    char m[96];
                    snprintf(m, sizeof m, "E=%d refract=%d G: rim disp %d, want %d",
                             E, refract, glass_disp[1][0], refract);
                    ck(glass_disp[1][0] == refract, m);
                }

                for (int s = E + 1; s <= GLASS_E_MAX; s++) {
                    char m[96];
                    snprintf(m, sizeof m, "E=%d %s: disp[%d]=%d past the band, want 0",
                             E, cn[c], s, glass_disp[c][s]);
                    ck(glass_disp[c][s] == 0, m);
                }
            }
        }
    }

    /* Dispersion, as an ordering rather than a magnitude: blue must never bend
     * less than red, and somewhere in a band wide enough to resolve it, it must
     * bend MORE -- otherwise the three samples collapse and the fringe that is
     * half the point of this material is not there.
     *
     * This is what -DGLASS_NO_DISPERSION breaks, and only this. */
    glass_build_lut(22, 18);
    int spread = 0;
    for (int s = 0; s <= 22; s++) {
        char m[96];
        snprintf(m, sizeof m, "E=22: B(%d)=%d < R=%d at s=%d",
                 s, glass_disp[2][s], glass_disp[0][s], s);
        ck(glass_disp[2][s] >= glass_disp[0][s], m);
        if (glass_disp[2][s] > glass_disp[0][s]) spread++;
    }
    ck(spread > 0, "E=22 refract=18: R and B never separate -- no dispersion");
    /* Where the fringe should be widest, it must be wide enough to SEE. One
     * pixel of separation is one pixel of fringe, which at a soft blurred edge
     * is nothing; this is the assertion that the per-channel normalisation bug
     * passed and this version does not. */
    {
        char m[96];
        snprintf(m, sizeof m, "E=22 refract=18: rim spread only %d px (R=%d B=%d)",
                 glass_disp[2][0] - glass_disp[0][0], glass_disp[0][0], glass_disp[2][0]);
        ck(glass_disp[2][0] - glass_disp[0][0] >= 2, m);
    }
    printf("  dispersion at E=22 refract=18: R %d, G %d, B %d px; separated at %d of 23 depths\n",
           glass_disp[0][0], glass_disp[1][0], glass_disp[2][0], spread);

    /* The shape claim from glass.c's table, asserted rather than asserted-in-
     * prose: at the middle of the bevel the physical curve is far below the
     * squared ramp it replaced. If someone reverts the curve this fires. */
    glass_build_lut(100, 1000);            /* clamps to GLASS_E_MAX */
    {
        int E = GLASS_E_MAX;
        int s = E / 2;                     /* u = 0.5 */
        double u = (double)(E - s) / E;
        int got = glass_disp[1][s];
        int ramp = (int)(1000 * u * u);
        char m[128];
        snprintf(m, sizeof m, "u=%.2f: physical %d px vs old squared ramp %d px -- not below it",
                 u, got, ramp);
        ck(got < ramp * 3 / 4, m);
        printf("  at u=%.2f of a 1000px bend: physical %d, old ramp %d (%.0f%% too strong)\n",
               u, got, ramp, 100.0 * (ramp - got) / got);
    }

    printf("worst fixed-point error: %.4f px (E=%d refract=%d %s s=%d)\n",
           worst, worst_e, worst_r, cn[worst_c], worst_s);
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
