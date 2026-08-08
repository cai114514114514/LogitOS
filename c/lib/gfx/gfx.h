#ifndef GFX_H
#define GFX_H
/* ============================================================================
 * Open Logit -- the 2D rendering engine.
 *
 * WHY IT EXISTS. Before this file there were three coverage/paint paths in the
 * tree: the kernel's M14 glyph rasterizer (c/kernel/gui/raster.c, glyphs only),
 * a second coverage rasterizer inside the widget toolkit (c/apps/gui/aui.c) and
 * a third hand-rolled paint path in the browser (c/apps/browser/browser_paint.c)
 * -- plus every new app starting from gui_rect. Open Logit is the one engine
 * those are built on. It is a LIBRARY, ring 3, beside c/lib/text and
 * c/lib/image, and it CONSUMES them: it rasterizes no glyph and decodes no
 * image. Blurring that line is how you end up with a fourth rasterizer instead
 * of the first engine.
 *
 * PHASE 1 IS FILL ONLY. paths (move/line/quad/cubic/close), nonzero + evenodd,
 * scanline coverage antialiasing, four paints (solid / linear gradient / radial
 * gradient / image), Porter-Duff src-over, an affine transform applied to paths,
 * and a rectangle clip. Stroking and path clipping are phase 2 and are not
 * started; see the note at the bottom of this header for what they will cost.
 *
 * THREE TECHNIQUES CARRIED OVER FROM THE TOOLKIT, because they are why it is
 * cheap enough to run a desktop on:
 *
 *   1. Masks are generated at DEVICE resolution and blitted into a POINT rect
 *      of the same device size, so the compositor's nearest-neighbour rescale
 *      is the identity and the antialiasing survives at 150% and 200%. Generate
 *      at 1x and stretch and you have a blurry mask of a sharp shape.
 *   2. Only what CURVES is rasterized. A rounded rect is a 9-slice: three
 *      opaque bands plus four r x r corner tiles, so it costs O(r^2) and not
 *      O(w*h). A gradient is one 1-px strip the compositor replicates. A
 *      uniform alpha fill is one 1x1 RGBA pixel stretched.
 *   3. Masks are cached by their exact device geometry.
 *
 * NO LIBC, NO MALLOC. Six GUI apps link this and nothing else but crt0 and
 * logit.h -- there is no memset, no memcpy and no allocator to reach for. Every
 * buffer is caller-provided or a bounded static; every bulk clear goes through
 * gfx_zero(), which writes through a volatile pointer precisely so that -O2's
 * loop-idiom pass cannot rewrite it into a call to memset that would then fail
 * to link.
 *
 * INTEGER ONLY. Coordinates are 24.8 fixed point in DEVICE pixels, matrices are
 * 16.16. That is not nostalgia: it is the same arithmetic the kernel's glyph
 * rasterizer uses, so a shape and the type beside it are antialiased by the same
 * rule and agree at the same size.
 * ========================================================================== */

/* ------------------------------------------------------------ fixed point -- */
#define GFX_ONE   256          /* coordinates: 24.8, device pixels           */
#define GFX_MONE  65536        /* matrix coefficients: 16.16                 */

/* Whole device pixels -> path coordinates. */
#define GFX_PX(n) ((int)(n) * GFX_ONE)

/* Sub-scanlines per pixel row. 4 is the kernel glyph rasterizer's number and
 * therefore this engine's: vertical error is bounded by 1/8 of a pixel of
 * coverage while horizontal coverage stays exact, which measures at a worst
 * error of 0.08 against a 16x supersampled reference. Raise it for a quality
 * experiment; the cost is linear. GFX_NO_AA drops it to a single centre sample
 * with binary horizontal coverage and IS the negative control: the accuracy
 * assertions must fail under it. */
#ifdef GFX_NO_AA
#define GFX_SUBS 1
#else
#ifndef GFX_SUBS
#define GFX_SUBS 4
#endif
#endif

/* ---------------------------------------------------------------- limits -- */
#define GFX_MAX_EDGES  1536    /* edges considered in one fill              */
#define GFX_MAX_ACTIVE 512     /* edges crossing one scanline               */
#define GFX_MAX_W      4096    /* device pixels across one fill             */
#define GFX_MAX_STOPS  8       /* gradient stops                            */

/* ----------------------------------------------------------------- misc -- */
void gfx_zero(void *p, int n);          /* memset(p,0,n) with no libc        */
unsigned long gfx_isqrt(unsigned long long v);
int gfx_sin(int deg256);                /* 16.16 sine of an angle in 24.8 degrees */
int gfx_cos(int deg256);

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

/* ------------------------------------------------------------------ path --
 * A path is built in USER coordinates and stored FLATTENED IN DEVICE SPACE:
 * the current matrix is applied as points are recorded, and curves are
 * subdivided against a device-space tolerance. That ordering is deliberate --
 * flattening in user space and transforming afterwards makes the segment count
 * a function of the wrong scale, so a shape enlarged 4x becomes visibly
 * polygonal. The cost is that re-transforming means rebuilding, which for an
 * immediate-mode UI is what happens anyway.
 *
 * Storage is the caller's. Nothing here allocates:
 *
 *     static int   pts[256 * 2];
 *     static int   subs[16];
 *     struct gfx_path p;
 *     gfx_path_init(&p, pts, 256, subs, 16);
 */
enum { GFX_NONZERO = 0, GFX_EVENODD = 1 };

struct gfx_path {
    int *pt;             /* x,y interleaved, 24.8 DEVICE                     */
    int  npt, ptcap;
    int *sub;            /* point index where each subpath starts            */
    int  nsub, subcap;
    int  overflow;       /* storage ran out; a fill is then refused outright  */
    struct gfx_matrix m; /* CTM applied as points are recorded               */
    int  cx, cy;         /* current point, device 24.8                       */
    int  sx, sy;         /* start of the open subpath                        */
    int  open;
    int  tol;            /* flattening tolerance, 24.8 device px             */
};

void gfx_path_init(struct gfx_path *p, int *ptbuf, int ptcap, int *subbuf, int subcap);
void gfx_path_reset(struct gfx_path *p);          /* keeps the matrix + tolerance */
void gfx_path_matrix(struct gfx_path *p, const struct gfx_matrix *m);
void gfx_path_tolerance(struct gfx_path *p, int tol256);

void gfx_move_to (struct gfx_path *p, int x, int y);
void gfx_line_to (struct gfx_path *p, int x, int y);
void gfx_quad_to (struct gfx_path *p, int cx, int cy, int x, int y);
void gfx_cubic_to(struct gfx_path *p, int c1x, int c1y, int c2x, int c2y, int x, int y);
void gfx_close   (struct gfx_path *p);

/* Shape sugar. Every one of these is ordinary path construction -- there is no
 * privileged rect or circle inside the rasterizer. */
void gfx_path_rect   (struct gfx_path *p, int x, int y, int w, int h);
void gfx_path_rrect  (struct gfx_path *p, int x, int y, int w, int h, int r);
void gfx_path_rrect4 (struct gfx_path *p, int x, int y, int w, int h,
                      int rtl, int rtr, int rbr, int rbl);
void gfx_path_ellipse(struct gfx_path *p, int cx, int cy, int rx, int ry);
void gfx_path_circle (struct gfx_path *p, int cx, int cy, int r);

/* Device-pixel bounding box (x0,y0 inclusive, x1,y1 exclusive). 0 if empty. */
int gfx_path_bounds(const struct gfx_path *p, int *x0, int *y0, int *x1, int *y1);

/* ------------------------------------------------------------------ clip --
 * Phase 1's clip is a rectangle, in device pixels. It is not a poor relation of
 * a path clip: a rect clip is exact, costs an intersection, and is what every
 * scroller and every window actually needs. */
struct gfx_rect { int x, y, w, h; };

/* --------------------------------------------------------------- surface --
 * An RGBA8 tile with STRAIGHT (non-premultiplied) alpha, matching what
 * SYS_GUI_BLIT consumes -- so a rendered surface goes to the screen with one
 * blit and no conversion pass. The engine never reads the window back (ring 3
 * cannot), so a surface is always something the caller owns and hands over. */
struct gfx_surface { unsigned char *px; int w, h, stride; };

void gfx_surface_init(struct gfx_surface *s, unsigned char *px, int w, int h, int stride);
void gfx_surface_clear(struct gfx_surface *s);

/* ----------------------------------------------------------------- paint -- */
enum { GFX_SOLID = 0, GFX_LINEAR, GFX_RADIAL, GFX_IMAGE };

struct gfx_stop { int t; unsigned color; int alpha; };   /* t: 0..65536 */

struct gfx_paint {
    int kind;
    unsigned color;  int alpha;              /* GFX_SOLID                     */
    struct gfx_stop stop[GFX_MAX_STOPS];
    int nstop;
    int x0, y0, x1, y1;                      /* GFX_LINEAR axis, device 24.8  */
    int cx, cy, r;                           /* GFX_RADIAL, device 24.8       */
    const unsigned char *img;                /* GFX_IMAGE, straight RGBA      */
    int iw, ih, istride;
    struct gfx_matrix inv;                   /* device -> image space         */
    int bilinear;
    int global_alpha;                        /* 0..255, multiplies the lot    */
};

void gfx_paint_solid (struct gfx_paint *p, unsigned color, int alpha);
void gfx_paint_linear(struct gfx_paint *p, int x0, int y0, int x1, int y1);
void gfx_paint_radial(struct gfx_paint *p, int cx, int cy, int r);
void gfx_paint_stop  (struct gfx_paint *p, int t, unsigned color, int alpha);
/* `img2dev` maps image pixels (24.8) to device (24.8); it is inverted here, so
 * a singular matrix is refused with 0 rather than dividing by zero per pixel. */
int  gfx_paint_image (struct gfx_paint *p, const unsigned char *rgba,
                      int w, int h, int stride,
                      const struct gfx_matrix *img2dev, int bilinear);
/* Sample the paint's colour ramp at t (0..65536). Exposed because the 1-pixel
 * gradient strip trick needs it and so do tests. */
unsigned gfx_paint_sample(const struct gfx_paint *p, int t, int *alpha_out);

/* ------------------------------------------------------------------ fill --
 * The two ways a path reaches pixels.
 *
 * gfx_fill_mask() produces 8-bit COVERAGE at device resolution -- that is the
 * cheap road: one colour, one gui_blit, no per-pixel source. It is what the
 * toolkit's corners, rings and shadows use.
 *
 * gfx_fill() composites a PAINT through that same coverage into an RGBA
 * surface with Porter-Duff src-over. Same rasterizer, same spans; only the
 * span consumer differs. */
int gfx_fill_mask(const struct gfx_path *p, int rule,
                  unsigned char *cov, int w, int h, int ox, int oy);
int gfx_fill(struct gfx_surface *dst, const struct gfx_path *p, int rule,
             const struct gfx_paint *paint, const struct gfx_rect *clip);

/* Composite one straight-RGBA source pixel over one straight-RGBA destination
 * pixel, src-over, with `cov` (0..255) modulating the source alpha. */
void gfx_over(unsigned char *dst, int sr, int sg, int sb, int sa, int cov);

/* ------------------------------------------------------------ mask cache --
 * Keyed by exact DEVICE geometry, so a frame full of controls that share a
 * radius rasterizes one quadrant and blits it 4n times.
 *
 * GFX_MASK_SHADOW is not a rasterization at all -- it is an analytic distance
 * ramp, because a shadow's alpha is a blur and not a coverage. It lives here
 * because it shares the cache, not because it shares the scanline loop. */
enum { GFX_MASK_FILL = 1, GFX_MASK_RING = 2, GFX_MASK_SHADOW = 3 };
#define GFX_MASK_MAX 72        /* largest cached tile edge, device px */

const unsigned char *gfx_mask_corner(int kind, int w, int h, int param);
void gfx_mask_stats(int *hits, int *misses);      /* for the cost tests */

/* Corner generators, callable directly when the caller does its own caching. */
void gfx_corner_fill  (unsigned char *m, int w, int h);
void gfx_corner_ring  (unsigned char *m, int w, int h, int t);
void gfx_corner_shadow(unsigned char *m, int w, int h, int r);
/* The shadow's alpha profile: 255 at the caster's edge, quadratic ease-out to 0
 * at `blur`. Exposed because a shadow's four corners are tiles but its four
 * EDGES are 1-pixel strips the compositor replicates, and both have to fall off
 * by the same curve or the corners show as seams. */
int gfx_shadow_falloff(long d256, long blur256);

/* Expand a coverage tile into straight RGBA for a blit, optionally mirrored --
 * which is how ONE rasterized quadrant serves all four corners. */
void gfx_mask_to_rgba(unsigned char *dst, const unsigned char *cov, int w, int h,
                      unsigned color, int alpha, int flipx, int flipy);

/* The 1-pixel gradient strip. `n` entries of straight RGBA written to dst; the
 * compositor replicates along the constant axis, so a gradient costs what a
 * flat fill costs. */
void gfx_gradient_strip(unsigned char *dst, int n, unsigned c0, unsigned c1, int alpha);
void gfx_gradient_strip_paint(unsigned char *dst, int n, const struct gfx_paint *p);

/* ---------------------------------------------------------------- colour -- */
unsigned gfx_mix(unsigned a, unsigned b, int t);      /* t 0..255 -> b */
#define GFX_R(c) (int)(((c) >> 16) & 255)
#define GFX_G(c) (int)(((c) >> 8) & 255)
#define GFX_B(c) (int)((c) & 255)
#define GFX_RGB(r,g,b) ((unsigned)(((r) << 16) | ((g) << 8) | (b)))

/* ============================================================================
 * PHASE 2, and what it will cost -- written down now so the API above does not
 * quietly foreclose it.
 *
 * STROKE. Offset curves have no closed form, so a stroker either flattens then
 * offsets the polyline (joins and caps become explicit geometry) or fills the
 * stroke's outline as a path. This engine already flattens into device-space
 * polylines, so the second road is open: emit the offset outline as two
 * subpaths wound oppositely and fill it NONZERO. The work is joins (miter with
 * a limit, round, bevel), caps (butt/round/square), and the degenerate cases --
 * a zero-length subpath with round caps is a dot, a cusp inside a curve
 * reverses the offset direction. The trap is already on record: aui's stroked
 * corner is an ELLIPSE SHARING THE OUTER'S CENTRE with radii reduced by the
 * width. Insetting the centre instead -- the mistake that looks right -- pinches
 * the arc to nothing before it meets the straight edges, and test-aui-mask
 * caught exactly that.
 *
 * PATH CLIPPING. The rasterizer already produces coverage; clipping to a path
 * is multiplying two coverages. The cheap version is a clip MASK -- rasterize
 * the clip path once into an 8-bit buffer and multiply spans through it -- which
 * is a small change to the span consumer and a large change to who owns the
 * buffer, since nothing here allocates. The expensive version is intersecting
 * span lists, which is exact and stateless but needs the clip's spans on hand
 * for every row of every fill.
 * ========================================================================== */

#endif /* GFX_H */
