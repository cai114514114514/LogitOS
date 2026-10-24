#ifndef LOGIT_CSS_GRADIENT_H
#define LOGIT_CSS_GRADIENT_H

enum { CG_NONE=0, CG_LINEAR, CG_RADIAL, CG_CONIC };
enum { CG_DIR_ANGLE=0, CG_DIR_CORNER };
/* Keep the original numeric order; old host consumers also use these IDs. */
enum { CG_CORNER_TL=0, CG_CORNER_TR, CG_CORNER_BR, CG_CORNER_BL };
enum { CG_POS_AUTO=0, CG_POS_PCT, CG_POS_PX, CG_POS_TURN };
enum { CG_ELLIPSE=0, CG_CIRCLE };
enum { CG_FARTHEST_CORNER=0, CG_CLOSEST_SIDE, CG_FARTHEST_SIDE,
       CG_CLOSEST_CORNER, CG_EXPLICIT };

/* Old claim: eight stops == GFX_MAX_STOPS. Correction: the browser sampler
 * now consumes its own stop list, including hints/two-position expansions;
 * using the GFX cap would silently truncate otherwise accepted gradients. */
#define CG_MAXSTOP 32
struct cg_length { double px, percent; }; /* percent is a fraction, not 0..100 */
struct cgstop {
    const char *color;
    int colorlen;
    int pos_kind;
    /* PCT retains its old unit, 10000 == 100%. PX is precise pixels; TURN is
     * a signed rotation, never modulo one: off-line stops affect interpolation.
     * AUTO is resolved only after image geometry exists (CSS Images fixup). */
    double pos;
    unsigned char hint, current_color;
};
struct cgradient {
    int kind, dir, corner, angle_mdeg;
    int nstop;
    struct cgstop stop[CG_MAXSTOP];
    int repeating, shape, extent;
    /* Circle percentage radius uses the box's scaled diagonal; ellipse
     * percentage radii use the corresponding width/height. */
    struct cg_length center[2], radius[2];
};

/* Strict ONE image value, borrowed color spans into v. fs/root resolve em/rem
 * only; geometry, fixup, premultiplied interpolation and repetition belong to
 * the painter. Never salvage a valid prefix from a trailing layer/garbage. */
int css_gradient_parse(const char *v,int len,int fs_px,int root_px,struct cgradient *out);
int css_gradient_supports(const char *v,int len);
#endif
