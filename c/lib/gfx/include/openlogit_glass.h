/* The rim refraction profile of the Liquid Glass material.
 *
 * This is its own translation unit for one reason: it is the only part of
 * fb.c that has a checkable right answer. Coverage has an analytic truth and
 * c/lib/gfx is verified against it, but a refraction curve's "truth" IS the
 * formula, so checking the formula against itself proves nothing. What CAN be
 * checked is the fixed-point arithmetic -- that this integer table agrees with
 * a double-precision evaluation of the same closed form. That check needs to
 * compile on the host, and fb.c cannot (framebuffer, VMM, the whole kernel).
 * Splitting the maths out costs one header and buys a real test.
 *
 * Derivation, provenance and the reason the old squared ramp was wrong are in
 * glass.c -- read that before changing any constant here. */
#ifndef LOGIT_GLASS_H
#define LOGIT_GLASS_H

/* Widest edge band the table is built for. fb_liquid_glass clamps E to the
 * panel's half-extent, so a small panel uses fewer entries; nothing indexes
 * past this. */
#define GLASS_E_MAX 24

/* Displacement in pixels for R, G, B at each inward distance 0..GLASS_E_MAX.
 * Index 0 is the outermost pixel of the rim. Rebuilt only when (E, refract)
 * changes, which in practice is once per panel geometry. */
extern int glass_disp[3][GLASS_E_MAX + 1];

/* Fresnel reflectance, 0..255, at each inward distance. Built by the same call
 * and cached on the same key, because it is a function of the same u. It is
 * what makes the panel have an EDGE rather than fading into the wallpaper --
 * see the note above glass_schlick in glass.c. */
extern unsigned char glass_fres[GLASS_E_MAX + 1];

void glass_build_lut(int E, int refract);

/* Schlick's approximation at incidence parameter u (16.16), as 0..255.
 * Exposed for the host test alongside glass_refract_ratio. */
/* The cap glass_schlick() clamps its output to. Here rather than in glass.c
 * because the TEST needs it: an oracle that does not model the cap reports the
 * capped ring as 26 failures against a curve the implementation never claimed
 * to follow at that point. The long argument for the NUMBER stays above the
 * clamp in glass.c -- this is the value, that is the reason. */
#define GLASS_FRES_MAX 190

int glass_schlick(int u);

/* a/(h+d) at incidence parameter u (16.16), refractive index en/ed, as 16.16.
 * Exposed for the host test, which compares it against the same expression in
 * double; nothing else should need it. */
int glass_refract_ratio(int u, int en, int ed);

/* Integer square root. It lives here rather than in fb.c because glass.c needs
 * it and glass.c is the file that gets compiled on the host -- one copy, in
 * the TU that is tested. */
unsigned gl_isqrt(unsigned long v);

#endif
