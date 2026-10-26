#ifndef GFX_MATH_H
#define GFX_MATH_H

/* gfx_math.h -- fixed point, rect and matrix. Includes nothing. */

/* ------------------------------------------------------------ fixed point -- */
#define GFX_ONE   256          /* coordinates: 24.8, device pixels           */
#define GFX_MONE  65536        /* matrix coefficients: 16.16                 */

/* Whole device pixels -> path coordinates. */
#define GFX_PX(n) ((int)(n) * GFX_ONE)

/* ----------------------------------------------------------------- misc -- */
void gfx_zero(void *p, int n);          /* memset(p,0,n) with no libc        */
unsigned long gfx_isqrt(unsigned long long v);
int gfx_sin(int deg256);                /* 16.16 sine of an angle in 24.8 degrees */
int gfx_cos(int deg256);

/* ------------------------------------------------------------- easing -----
 * The desktop's motion curves, on 0..256 in and 0..256 out. Here, in the one
 * library BOTH RINGS link, because the window manager (ring 0) and the widget
 * toolkit (ring 3) animate side by side on the same screen and a curve spelled
 * twice agrees on the wrong shape about as often as the right one. See the
 * definitions in gfx_math.c for why the domain is 0..256 and not 0..255. */
int gfx_ease_out(int t);                /* quadratic decel: the default        */
int gfx_ease_inout(int t);              /* smoothstep: rest at both ends       */

/* ---------------------------------------------------------------- matrix --
 *   | a c e |    x' = (a*x + c*y) / 65536 + e
 *   | b d f |    y' = (b*x + d*y) / 65536 + f      x, y, e, f in 24.8
 *
 * This is the piece that CSS `transform` needs -- `translate(-50%,-50%)`
 * centring is reached by 14 of 15 real pages -- so the transform is applied to
 * PATHS and composes, rather than being a special case bolted onto a rect. */
struct gfx_matrix { int a, b, c, d, e, f; };

void gfx_m_identity(struct gfx_matrix *m);
void gfx_m_set(struct gfx_matrix *m, int a, int b, int c, int d, int e, int f);
/* o = l * r  (apply r first, then l -- the usual CTM composition order). */
void gfx_m_mul(struct gfx_matrix *o, const struct gfx_matrix *l, const struct gfx_matrix *r);
void gfx_m_translate(struct gfx_matrix *m, int dx, int dy);    /* 24.8   */
void gfx_m_scale(struct gfx_matrix *m, int sx, int sy);        /* 16.16  */
void gfx_m_rotate(struct gfx_matrix *m, int deg256);           /* 24.8 degrees */
int  gfx_m_invert(struct gfx_matrix *o, const struct gfx_matrix *m);  /* 0 if singular */
void gfx_m_apply(const struct gfx_matrix *m, int x, int y, int *ox, int *oy);
int  gfx_m_scale_of(const struct gfx_matrix *m);   /* approx uniform scale, 16.16 */

/* ------------------------------------------------------------------ clip --
 * Phase 1's clip is a rectangle, in device pixels. It is not a poor relation of
 * a path clip: a rect clip is exact, costs an intersection, and is what every
 * scroller and every window actually needs. */
struct gfx_rect { int x, y, w, h; };

#endif /* GFX_MATH_H */
