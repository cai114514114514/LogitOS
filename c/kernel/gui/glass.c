/* ---- the rim's refraction profile ----------------------------------------
 *
 * WHAT THIS REPLACES, and why the old one was wrong in a specific way. The
 * displacement used to be `REFRACT * (1-t)^2` -- a squared ramp, picked because
 * it is two multiplies and looks plausible. Against the actual optics it is 50
 * to 80 percent too strong through the middle of the bevel:
 *
 *      u        (1-t)^2      physical       ours was
 *      0.5        0.250        0.167          +50%
 *      0.7        0.490        0.268          +83%
 *      0.9        0.810        0.463          +75%
 *      1.0        1.000        1.000           equal
 *
 * and the SHAPE of that error is the whole difference, not the magnitude. The
 * real curve is nearly flat across the bevel and then runs away in the last
 * pixel or two, because tan() climbs steeply as the incidence angle approaches
 * grazing. Spreading the bend evenly over a 22-pixel band reads as a soft
 * vignette. Concentrating it at the rim reads as THICK GLASS -- which is the
 * thing about this material that people actually recognise, and it comes from
 * the curve, not from adding more highlight.
 *
 * THE MODEL. A quarter-round bevel of radius t on a slab, the backdrop (h+d)
 * below. At inward distance s the normal's tilt is fixed by position on the
 * arc, so with u = (t-s)/t the incidence angle is phi = asin(u), Snell gives
 * sin(phi') = u/eta, and the ray lands a = (h+d)*tan(phi - phi') away.
 * Expanding tan(phi-phi') with sin phi = u, cos phi = sqrt(1-u^2),
 * sin phi' = u/eta, cos phi' = sqrt(1-u^2/eta^2) removes every trig function:
 *
 *                u*sqrt(1-u^2/eta^2) - sqrt(1-u^2)*(u/eta)
 *     a/(h+d) = -------------------------------------------
 *                sqrt(1-u^2)*sqrt(1-u^2/eta^2) + u^2/eta
 *
 * Two square roots, no trig, no float -- which is the only reason this can be
 * in a kernel at all. It does not diverge at the rim: Snell caps phi' at
 * asin(1/eta), so a(1) is finite (1.118*(h+d) at eta=1.5). Rather than pick
 * (h+d), the table is NORMALISED so a(1) == REFRACT, i.e. the panel's existing
 * visual constant still means "displacement at the outermost pixel" and nothing
 * had to be retuned.
 *
 * DISPERSION. Blue refracts more than red, so R, G and B are sampled at three
 * different displacements. This is the one thing the material has that we had
 * none of, and it is the most recognisable half of the look. Since a(1) works
 * out to sqrt(eta^2 - 1), the rim spread is fixed entirely by the fan: eta is
 * fanned +-4% about 1.5, giving 0.93 : 1 : 1.07 and therefore 16.7 : 18 : 19.3
 * at REFRACT=18 -- 17, 18 and 19 px once rounded, so two whole pixels of
 * red-to-blue separation at the rim, which is the narrowest fringe that reads
 * as one. Real glass disperses about 0.5% across
 * the visible band and would move the fringe by a fifth of a pixel, so this is
 * an artistic exaggeration, chosen against a number, and written down as one.
 *
 * COST. Strictly cheaper than what it replaces: the old ramp was two multiplies
 * and a shift per pixel, this is one array index, and the table is at most 25
 * entries built once per panel geometry and cached across frames. Dispersion
 * costs two extra unpacks, and only inside the edge band.
 *
 * PROVENANCE, because this one has a trap in it. Derived from the published
 * technique, not from anyone's code: Snell's law over a circular bevel is not
 * copyrightable, and MetroWind published the derivation as prose and formulae
 * at blog.mws.rocks/p/96. The only C repository implementing it
 * (ringprod/glass-distortion) ships an MIT file over a shader its own README
 * credits to a Shadertoy work -- whose default terms are CC BY-NC-SA, which it
 * had no right to re-grant. No line of it is here, nothing third-party entered
 * the tree, and THIRD_PARTY.md therefore needs no entry. */
#include "glass.h"

int glass_disp[3][GLASS_E_MAX + 1];
unsigned char glass_fres[GLASS_E_MAX + 1];

static int glass_lut_e = -1;                       /* the E these were built for */
static int glass_lut_r = -1;                       /* ...and the REFRACT */

unsigned gl_isqrt(unsigned long v)
{
    unsigned long r = 0, b = 1UL << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (unsigned)r;
}

/* sqrt of a 16.16 value, as 16.16. */
static unsigned sqrt16(unsigned long v)
{
    return (unsigned)gl_isqrt(v << 16);
}

int glass_refract_ratio(int u, int en, int ed)
{
    long long u2 = ((long long)u * u) >> 16;                 /* u^2        */
    if (u2 > 65536) u2 = 65536;
    long long ue = (long long)u * ed / en;                   /* u/eta      */
    long long u2e = (ue * ue) >> 16;                         /* u^2/eta^2  */
    if (u2e > 65536) u2e = 65536;

    long long ca = sqrt16((unsigned long)(65536 - u2));      /* cos phi    */
    long long cb = sqrt16((unsigned long)(65536 - u2e));     /* cos phi'   */

    long long num = ((long long)u * cb - ca * ue) >> 16;
    long long den = ((ca * cb) >> 16) + u2 * ed / en;        /* + u^2/eta  */
    if (den <= 0) return 0;
    if (num <= 0) return 0;
    return (int)((num << 16) / den);
}

/* ---- the rim's REFLECTANCE, which is the part that was missing -------------
 *
 * The refraction curve above decides where the rim looks; this decides how much
 * of the rim is looking at all. They are the two halves of the same interface
 * and only one of them was implemented, which is why the panel had no edge.
 *
 * THE SYMPTOM, stated first because it is what you actually see. The dock's
 * border was a 20-pixel soft gradient with NO LINE IN IT. A real piece of glass
 * reads as a solid object with thickness because its outermost pixel is a
 * bright hairline; ours dissolved into the wallpaper. The existing specular was
 * not that line -- it is a wide wash spread over eband = 15 px, which is a soft
 * vignette by construction, and no amount of raising SPEC turns a 15-pixel
 * gradient into a 1-pixel edge. It also lit only the side facing the light, so
 * three quarters of the outline had nothing at all.
 *
 * WHY IT IS A HAIRLINE AND NOT A TASTE DECISION. Reflectance rises toward
 * grazing incidence and at exactly grazing it is 1: the outermost pixel of a
 * bevel is a MIRROR. Schlick's approximation, R = R0 + (1-R0)(1-cos)^5 with
 * R0 = ((eta-1)/(eta+1))^2 = 0.04, is the standard cheap form of that, and the
 * fifth power is why the falloff is so abrupt. At E = 22 the table reads
 *
 *      s  =    0     1     2     3     4  ...  interior
 *      R  =  255    52    27    18    14  ...     10
 *
 * -- one blazing pixel, two dim ones, then a 4% sheen across the whole face,
 * which is also correct: flat glass viewed head-on reflects four percent, and
 * that faint lift over the entire panel is a second cue we did not have.
 *
 * WHAT THE RIM MIRRORS is faked, and this is the one place here that is not
 * physics. Reflecting the true surroundings needs a second sample of what is
 * OUTSIDE the panel, and this function only ever copied the panel's own
 * rectangle -- so the environment is a fixed vertical gradient, bright toward
 * the same up-left light the specular already assumes, and never black:
 * ambient light has no zero, and a rim that reflects black at the bottom reads
 * as a dark outline rather than glass. Written down as the cheat it is.
 *
 * The lerp toward that environment is energy-conserving by construction --
 * what is reflected is not transmitted -- so the darker line just inside the
 * bright one comes out for free, and that pair IS the thickness cue. */
int glass_schlick(int u)
{
    long long u2 = ((long long)u * u) >> 16;
    if (u2 > 65536) u2 = 65536;
    long long c = sqrt16((unsigned long)(65536 - u2));       /* cos theta  */
    long long m = 65536 - c;
    if (m < 0) m = 0;
    /* Rounded, not truncated, at every step: a fifth power truncates four
     * times and the errors all go the same way, which put one entry of the
     * table a whole level below the double reference. */
    long long m2 = (m * m + 32768) >> 16;
    long long m4 = (m2 * m2 + 32768) >> 16;
    long long m5 = (m4 * m + 32768) >> 16;                   /* (1-cos)^5  */
    long long R = 2621 + (((65536 - 2621) * m5 + 32768) >> 16);   /* R0 = 0.04 */
    if (R > 65536) R = 65536;
    return (int)((R * 255 + 32768) >> 16);
}

void glass_build_lut(int E, int refract)
{
    if (E == glass_lut_e && refract == glass_lut_r) return;  /* cached */
    if (E < 1) E = 1;
    if (E > GLASS_E_MAX) E = GLASS_E_MAX;

    /* eta fanned +-2% about 1.5. Blue bends most, so it takes the highest index
     * -- getting this backwards puts the red fringe on the wrong side, which is
     * a thing the eye notices without being able to say why. */
#ifdef GLASS_NO_DISPERSION
    static const int en[3] = { 150, 150, 150 };              /* negative control */
#else
    static const int en[3] = { 144, 150, 156 };              /* R, G, B */
#endif
    /* ONE normalisation constant, green's, for all three channels -- not each
     * channel's own peak. This is the whole of the dispersion and it is easy to
     * destroy: (h+d) is the same slab for every wavelength, so normalising per
     * channel divides the difference back out and pins all three to REFRACT at
     * the rim, which is exactly where the fringe should be widest. Done that
     * way the table still refracts correctly and the colours never separate --
     * it looks like a bug in the blur, not in the maths. */
    int peak = glass_refract_ratio(65536, en[1], 100);       /* a(1) for green */
    if (peak <= 0) peak = 1;

    for (int c = 0; c < 3; c++) {
        for (int s = 0; s <= E; s++) {
            int u = (E - s) * 65536 / E;                     /* 1 at the rim */
            long long r = glass_refract_ratio(u, en[c], 100);
            /* Rounded, not truncated: at 18 px the whole fringe is under two
             * pixels wide, so a systematic half-pixel bias downward is a
             * meaningful fraction of the effect. */
            glass_disp[c][s] = (int)((r * refract + peak / 2) / peak);
        }
        for (int s = E + 1; s <= GLASS_E_MAX; s++) glass_disp[c][s] = 0;
    }

    for (int s = 0; s <= E; s++)
        glass_fres[s] = (unsigned char)glass_schlick((E - s) * 65536 / E);
    /* Past the band the surface is flat, and flat glass still reflects R0.
     * Zeroing here would put a seam at s == E. */
    for (int s = E + 1; s <= GLASS_E_MAX; s++) glass_fres[s] = glass_fres[E];

    glass_lut_e = E;
    glass_lut_r = refract;
}
