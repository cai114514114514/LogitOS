/* css_interp.h -- CSS value interpolation, and `transform` as a value.
 *
 * WHAT THIS IS FOR, because the name undersells it.
 *
 * 337 files in css/ are driven by WPT's interpolation-testcommon.js, and they
 * all ask the same question in the same shape: given a property, two values,
 * and a progress p, what does getComputedStyle say? 47,140 subtest failures in
 * css/ come from those files -- 44% of every css/ failure -- and they are not
 * 47,140 defects. They are three:
 *
 *     35,708   CSS.supports(property, value) answers false
 *     10,714   'animate' in Element.prototype is false
 *        718   the interpolated value itself is wrong
 *
 * This file is the third one, and it is the part the other two need
 * underneath: a timeline that advances and a parser that accepts a value are
 * both worthless if the value at p = 0.37 is wrong. It is deliberately a
 * LIBRARY and not a policy -- no timers, no keyframe resolution, no DOM. It
 * takes two strings and a number and returns a string.
 *
 * THE ONE THING TO KNOW ABOUT TRANSFORM LISTS, which is the whole reason this
 * file is not fifty lines of lerp:
 *
 *     Two transform lists interpolate ITEM BY ITEM when their function lists
 *     match, and by MATRIX DECOMPOSITION when they do not.
 *
 * Componentwise-always is the plausible wrong implementation and it is
 * invisible: every animation between two translate()s is perfect, every page
 * looks right, and rotate() against scale() gets an answer that is smooth,
 * finite and wrong. tests/interp.mk builds exactly that as -DCI_NEGCTL_NO_DECOMPOSE
 * and requires the suite to catch it.
 *
 * Column-major, flat: m[col * 4 + row], which is the order matrix3d() lists
 * its arguments in, so matrix3d(a1..a16) fills m[0..15] with no permutation.
 */
#ifndef CSS_INTERP_H
#define CSS_INTERP_H

/* ---- transform functions ------------------------------------------------ */
enum {
    CI_TRANSLATE = 0, CI_TRANSLATEX, CI_TRANSLATEY, CI_TRANSLATEZ, CI_TRANSLATE3D,
    CI_SCALE, CI_SCALEX, CI_SCALEY, CI_SCALEZ, CI_SCALE3D,
    CI_ROTATE, CI_ROTATEX, CI_ROTATEY, CI_ROTATEZ, CI_ROTATE3D,
    CI_SKEW, CI_SKEWX, CI_SKEWY,
    CI_MATRIX, CI_MATRIX3D, CI_PERSPECTIVE,
    CI__NFN
};

#define CI_MAXFN 24

/* One function. `a[]` holds its arguments already made ABSOLUTE: lengths in
 * px, angles in RADIANS, plain numbers as themselves.
 *
 * `pc[]` is the PERCENTAGE part of the same argument, carried alongside rather
 * than collapsed into it. Only translate() takes percentages, they resolve
 * against the element's border box which this file does not have, and -- the
 * reason for two fields and not a flag -- a length and a percentage that
 * interpolate against each other produce a calc(), so a value part-way through
 * such an animation is BOTH at once. A flag would have to pick one and would
 * be wrong at every progress except 0 and 1. */
struct ci_fn {
    unsigned char kind;
    unsigned char nargs;
    unsigned char haspc;
    double a[16];
    double pc[3];
};

struct ci_xform {
    int n;                          /* 0 = `none`, which is the identity */
    struct ci_fn f[CI_MAXFN];
};

/* Parse a <transform-list>. `fs_px` is the element's font-size, used for em;
 * `root_px` for rem. Returns 0, or -1 if the value is not a transform list.
 * `none` parses to n == 0 and is NOT an error. */
int ci_transform_parse(const char *s, int len, double fs_px, double root_px,
                       struct ci_xform *out);

/* Collapse to a 4x4. Percentages resolve against (refw, refh). */
void ci_transform_matrix(const struct ci_xform *t, double refw, double refh,
                         double m[16]);

/* The RESOLVED value: matrix(...) for a 2D matrix, matrix3d(...) otherwise.
 * This is what getComputedStyle returns for a rendered element. */
int ci_matrix_text(const double m[16], char *out, int outmax);

/* The COMPUTED value: the function list with its lengths made absolute. This
 * is what an element with `display: none` reports, and the two are different
 * strings for the same style -- WPT tests both, and the distinction is where a
 * plausible implementation fails. */
int ci_transform_text(const struct ci_xform *t, char *out, int outmax);

/* Interpolate two lists at progress p (which may be < 0 or > 1 -- WPT tests
 * -1 and 2 on purpose, and clamping there is a real failure mode). */
void ci_transform_interp(const struct ci_xform *a, const struct ci_xform *b,
                         double p, struct ci_xform *out);

/* ---- matrix decomposition ----------------------------------------------- */
struct ci_decomp {
    double translate[3];
    double scale[3];
    double skew[3];             /* xy, xz, yz */
    double perspective[4];
    double quaternion[4];       /* x, y, z, w */
};

int  ci_decompose(const double m[16], struct ci_decomp *d);
void ci_recompose(const struct ci_decomp *d, double m[16]);
void ci_decomp_interp(const struct ci_decomp *a, const struct ci_decomp *b,
                      double p, struct ci_decomp *out);

/* ---- generic values ----------------------------------------------------- */
/* Interpolate `from` and `to` for `prop` at p, writing a computed-form value.
 * Returns the length written, or -1 when the pair is not interpolable -- the
 * caller then applies the discrete rule (p < 0.5 ? from : to) itself, because
 * the flip point differs between animations and transitions and this file does
 * not know which one is asking. */
int ci_value_interp(const char *prop, const char *from, const char *to,
                    double p, char *out, int outmax);

/* Is this property's value type discrete? (`display`, `visibility` and the
 * rest flip rather than blend.) */
int ci_prop_is_discrete(const char *prop);

/* Serialize a double the way CSS does: up to six significant digits, no
 * exponent in the ordinary range, no trailing zeros, and never "-0". */
int ci_num_text(double v, char *out, int outmax);

/* Test seam: how many interpolations took the decomposition path rather than
 * the componentwise one. tests/unit/interp_test.c asserts this is NONZERO for
 * a mismatched pair -- a suite that only checks the output value cannot tell
 * componentwise-always from correct on the pairs where the two agree. */
int ci_decompose_count(void);
void ci_decompose_count_reset(void);

#endif
