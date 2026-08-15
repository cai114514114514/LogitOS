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
 * those are built on. It is a LIBRARY beside c/lib/text and c/lib/image, and it
 * CONSUMES them: it rasterizes no glyph and decodes no image. Blurring that
 * line is how you end up with a fourth rasterizer instead of the first engine.
 *
 * NOT ring-3-only, despite what this comment used to say. c/lib/gfx is filtered
 * out of C_SRC (kernel translation units) for c/lib/video, c/lib/audio,
 * c/lib/media and the image codecs -- but NOT for this directory. fb.c
 * (c/kernel/gui/fb.c) includes gfx.h and calls into gfx_mask_corner/
 * gfx_fill_mask for the window manager's own rounded corners, so this whole
 * file set is compiled into the KERNEL as well as into every ring-3 GUI
 * binary. That means every byte added to a static here is a byte of kernel
 * .bss too -- see the LIMITS section below for what that costs.
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

/* Sub-scanlines per pixel row. This USED to be a single compile-time constant
 * pinned to 4, on the claim that 4 is "the kernel glyph rasterizer's number and
 * therefore this engine's" -- that was false the day it was written: the glyph
 * rasterizer (c/kernel/gui/raster.c) uses 16 (SUB_SHIFT 4), not 4, and it uses
 * 16 for a measured reason -- at 4 a thin horizontal bar like an underscore,
 * whose top and bottom edges both land inside one sub-scanline band, comes out
 * 11/255 off FreeType on average; at 16 that drops to about 3. A shape and a
 * glyph are not the same job either: text is small, dense and cached once per
 * (glyph, size), a button is large and redrawn on every frame of a drag, so
 * paying 4x the vertical passes for every rounded rect to buy glyph-grade
 * accuracy nobody asked for would be the wrong trade the other direction.
 *
 * So the count is a PER-CALL argument (`subs`, plumbed through gfx_raster.c's
 * raster()) and GFX_SUBS below is only the DEFAULT the plain gfx_fill_mask()/
 * gfx_fill() names use, kept for the five call sites (aui.c, clock.c,
 * browser_paint.c, fb.c, the tests) that predate this and ask for neither text
 * nor an explicit count -- see gfx_fill_mask_subs()/gfx_fill_subs() below for
 * the callers that do (glyphs, once text is drawn through this engine, will
 * pass 16). GFX_MAX_SUBS bounds the argument so a bad caller can't turn one
 * fill into an unbounded number of scanline passes; it costs no .bss, unlike
 * the edge-table caps below, because nothing here is sized by it.
 *
 * GFX_NO_AA drops the EFFECTIVE count to a single centre sample with binary
 * horizontal coverage regardless of what a caller asks for, and IS the negative
 * control: the accuracy assertions must fail under it. */
#ifdef GFX_NO_AA
#define GFX_SUBS 1
#else
#ifndef GFX_SUBS
#define GFX_SUBS 4          /* default for gfx_fill_mask()/gfx_fill()        */
#endif
#endif
#define GFX_MAX_SUBS 32     /* ceiling on the per-call `subs` argument         */

/* ---------------------------------------------------------------- limits --
 * SIZED FOR TWO WORKLOADS THIS ENGINE DOES NOT SERVE YET: text-as-paths and
 * SVG, both scheduled in docs/superpowers/specs/2026-08-14-open-logit-2-design.md.
 * The old numbers (edges 1536, active 512) were sized for the corner quadrants
 * and simple shapes phase 1 actually draws (<= 64 segments) and silently broke
 * on anything bigger -- see gfx_raster.c's overflow handling for what "broke"
 * meant before this milestone.
 *
 * GFX_MAX_EDGES 8192 matches c/kernel/gui/raster.c's MAXEDGE exactly, which is
 * not a coincidence: that is the kernel's OWN glyph rasterizer, sized for a
 * CJK glyph at a large pixel size (Noto Sans SC, up to ~2.2 MB of glyf data),
 * and it is proven sufficient in production -- zh.wikipedia.org has rendered
 * through it since M14. When text moves onto this engine (a scheduled phase-2
 * step, "text as paths") it inherits exactly the number that already worked,
 * rather than a fresh guess. It is also generous for SVG path data: a
 * hand-drawn icon runs to dozens of segments, not thousands.
 *
 * GFX_MAX_ACTIVE 1024 matches the same rasterizer's MAXCROSS (crossings live
 * per sub-scanline there, since it has no persistent active-edge list and
 * rescans every edge each sub-scanline instead) -- the same proven-sufficient
 * number, reused rather than picked fresh, for the same reason. Concurrently
 * active edges can only be a fraction of total edges in real geometry (nothing
 * plausible has 1024 contours simultaneously open at one scanline), so this is
 * generous headroom over GFX_MAX_EDGES 8192, not a matching pair by
 * necessity.
 *
 * GFX_MAX_W is untouched at 4096: it bounds gfx_row's width, which is desktop
 * pixels wide (today's largest bench case is 2560), not glyph- or path-segment
 * related, and 4096 already clears every real display width with margin.
 *
 * COST: this is kernel .bss (see the note at the top of this file). The five
 * rasterizer statics (g_edge/g_order/g_act/g_cross/g_row in gfx_raster.c) were
 * 43,008 B at the old caps; at these caps they are 194,560 B -- see the
 * per-array arithmetic in gfx_raster.c's file comment. Plus the (unchanged)
 * mask cache (82,944 B) and corner-tile scratch (4,128 B), the engine's total
 * static footprint goes from about 130,080 B to about 281,632 B, a delta of
 * 151,552 B (~148 KiB). Raising these is a real cost paid by every kernel
 * boot, not a free number -- it earns its place only because it is the exact
 * number this OS already runs a CJK-glyph workload through today. */
#define GFX_MAX_EDGES  8192    /* edges considered in one fill              */
#define GFX_MAX_ACTIVE 1024    /* edges crossing one scanline               */
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
    int  overflow;       /* storage ran out, OR gfx_path_matrix() was called
                          * mid-path (see its declaration below) -- either way
                          * a fill is then refused outright rather than drawn
                          * from a path that cannot be trusted                */
    struct gfx_matrix m; /* CTM applied as points are recorded               */
    int  cx, cy;         /* current point, device 24.8                       */
    int  sx, sy;         /* start of the open subpath                        */
    int  open;
    int  tol;            /* flattening tolerance, 24.8 device px             */
};

void gfx_path_init(struct gfx_path *p, int *ptbuf, int ptcap, int *subbuf, int subcap);
void gfx_path_reset(struct gfx_path *p);          /* keeps the matrix + tolerance */
/* Sets the CTM applied to every point recorded FROM HERE ON (path coordinates
 * are transformed and flattened as they are built -- see the path comment
 * above). Calling this after the path already holds a point is refused: the
 * points already recorded were flattened under the OLD matrix, so a path with
 * two calls to this mid-build would silently carry two different transforms in
 * two halves of the same shape -- a plausible-looking wrong picture, and per
 * this file's rule (gfx_path.c's file comment) that is refused, not drawn. The
 * refusal reuses `overflow`: to a caller of gfx_fill/gfx_fill_mask "ran out of
 * point storage" and "recorded under two different transforms" are the same
 * fact -- this path cannot be trusted -- so they share one flag and raster()
 * (gfx_raster.c) needs exactly one check for both. Calling it before the first
 * point (fresh from gfx_path_init/gfx_path_reset, including more than once)
 * is the normal case and always allowed. */
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
 * span consumer differs.
 *
 * Both come in a plain form, at the GFX_SUBS default, and a `_subs` form that
 * takes the sub-scanline count explicitly (see the GFX_SUBS comment above for
 * why that is a per-call choice and not one constant). The plain names are
 * kept, with their existing signature, because gfx_fill_mask/gfx_fill already
 * have callers in five files this milestone does not own (aui.c, clock.c,
 * browser_paint.c, fb.c, the tests) -- adding a variant rather than an
 * argument means none of those five needs a one-line edit to keep building. */
int gfx_fill_mask(const struct gfx_path *p, int rule,
                  unsigned char *cov, int w, int h, int ox, int oy);
int gfx_fill_mask_subs(const struct gfx_path *p, int rule,
                       unsigned char *cov, int w, int h, int ox, int oy, int subs);
int gfx_fill(struct gfx_surface *dst, const struct gfx_path *p, int rule,
             const struct gfx_paint *paint, const struct gfx_rect *clip);
int gfx_fill_subs(struct gfx_surface *dst, const struct gfx_path *p, int rule,
                  const struct gfx_paint *paint, const struct gfx_rect *clip, int subs);

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
/* Oversize refusals since boot. A NULL from gfx_mask_corner is a REFUSAL the
 * caller must handle, not a cache miss -- and it used to be invisible even to
 * gfx_mask_stats (mmiss++ sat after the early return). Counted separately so a
 * harness can assert "no shape silently degraded this frame". */
int  gfx_mask_refused(void);

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
 * PHASE 2 API -- DECLARATIONS ONLY. Nothing below this line is implemented by
 * this milestone (G0); it is the contract G1 (stroke) and G2 (path clip) code
 * against, published now so their signatures don't get designed twice. Calling
 * any of it links but is meaningless until the corresponding unit lands.
 *
 * STROKE. Offset curves have no closed form, so a stroker either flattens then
 * offsets the polyline (joins and caps become explicit geometry) or fills the
 * stroke's outline as a path. This engine already flattens into device-space
 * polylines, so the second road is open: emit the offset outline as two
 * subpaths wound oppositely and fill it NONZERO -- gfx_stroke_path() below
 * turns a path into a path; it does not rasterize, so the caller fills the
 * result exactly as it would any other path. The work is joins (miter with a
 * limit, round, bevel), caps (butt/round/square), dashing, and the degenerate
 * cases -- a zero-length subpath with round caps is a dot, a cusp inside a
 * curve reverses the offset direction. The trap is already on record: aui's
 * stroked corner is an ELLIPSE SHARING THE OUTER'S CENTRE with radii reduced by
 * the width. Insetting the centre instead -- the mistake that looks right --
 * pinches the arc to nothing before it meets the straight edges, and
 * test-aui-mask caught exactly that; whatever replaces aui's stroker had
 * better fail the same way if it repeats the mistake. */
enum { GFX_CAP_BUTT = 0, GFX_CAP_ROUND = 1, GFX_CAP_SQUARE = 2 };
enum { GFX_JOIN_MITER = 0, GFX_JOIN_ROUND = 1, GFX_JOIN_BEVEL = 2 };

struct gfx_stroke {
    int width;              /* full stroke width (not half), device 24.8    */
    int cap;                /* GFX_CAP_*, applied to every open subpath end  */
    int join;                /* GFX_JOIN_*                                    */
    int miter_limit;         /* 16.16; miter length / width beyond which a
                              * GFX_JOIN_MITER corner falls back to bevel     */
    const int *dash;        /* device 24.8 on,off,on,off,... or NULL = solid */
    int ndash;               /* length of `dash`; must be even if non-NULL    */
    int dash_phase;          /* device 24.8 offset into the pattern at the
                              * start of each subpath                        */
};

/* Appends the stroke outline of every subpath in `src` to `out` (which the
 * caller has already gfx_path_init'd with its own storage -- a stroke costs
 * roughly 2x src's point count plus join/cap geometry, more for round joins).
 * 0 on refusal: `out` overflowed (out->overflow is latched the normal way),
 * `src` overflowed, or `stroke->width` <= 0. Refuses rather than emitting a
 * partial outline, same rule as everything else in this file. */
int gfx_stroke_path(struct gfx_path *out, const struct gfx_path *src,
                    const struct gfx_stroke *stroke);

/* PATH CLIPPING. The rasterizer already produces coverage; clipping a fill to
 * a path is multiplying two coverages together. The multiply point is the
 * row_fn seam gfx_raster.c already has (the `fn` callback declared at :184,
 * called once per finished row at :274) -- but raster() is NOT REENTRANT (its
 * edge/active/crossing tables are file statics, gfx_raster.c:77-81), so a clip
 * path can never be rasterized from INSIDE a subject fill's own row callback.
 * The only sound design is therefore two separate rasterizations in sequence:
 * rasterize the clip path to a finished coverage buffer FIRST, with an
 * ordinary gfx_fill_mask() call, then hand that finished buffer to the
 * subject's fill so its row callback can multiply through it. `gfx_clip_mask`
 * is that finished buffer -- caller-owned, like every buffer in this file --
 * and gfx_fill_(mask_)clipped are the only entry points that accept one, which
 * is what makes "rasterize first, multiply second" the only sequence a caller
 * can express. */
#define GFX_CLIP_MASK_MAX 2048   /* largest w or h a clip coverage buffer may
                                  * cover, device px -- generous for one
                                  * window's content area; a bigger request is
                                  * refused loudly (G2), not clipped to fit */

struct gfx_clip_mask {
    const unsigned char *cov;    /* device coverage, 0..255, already rasterized
                                  * (e.g. by an earlier gfx_fill_mask call on
                                  * the clip path) -- this struct never
                                  * rasterizes anything itself             */
    int w, h;                    /* device pixels covered, each <= GFX_CLIP_MASK_MAX */
    int ox, oy;                  /* device origin: cov[0] is device pixel (ox,oy) */
};

/* Exactly gfx_fill_mask_subs()/gfx_fill_subs(), except every row's coverage is
 * multiplied by `clip` before it reaches the mask buffer / paint compositor. A
 * device pixel outside [ox,ox+w) x [oy,oy+h) of `clip` reads as coverage 0 --
 * clipped away -- rather than falling back to unclipped, so a caller cannot
 * under-size the clip buffer and get a bigger clip than it actually drew. */
int gfx_fill_mask_clipped(const struct gfx_path *p, int rule,
                          unsigned char *cov, int w, int h, int ox, int oy,
                          int subs, const struct gfx_clip_mask *clip);
int gfx_fill_clipped(struct gfx_surface *dst, const struct gfx_path *p, int rule,
                     const struct gfx_paint *paint, const struct gfx_rect *rectclip,
                     int subs, const struct gfx_clip_mask *clip);
/* ========================================================================== */

#endif /* GFX_H */
