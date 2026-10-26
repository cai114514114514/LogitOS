#ifndef GFX_RASTER_H
#define GFX_RASTER_H

/* gfx_raster.h -- surfaces, fill rules, the scanline rasterizer and its workspace,
 * src-over, and path clipping. Includes the math, path and paint headers. */

#include "gfx_math.h"
#include "gfx_path.h"
#include "gfx_paint.h"

/* Sub-scanlines per pixel row. This USED to be a single compile-time constant
 * pinned to 4, on the claim that 4 is "the kernel glyph rasterizer's number and
 * therefore this engine's" -- that was false the day it was written: the glyph
 * rasterizer (then c/kernel/gui/raster.c, now this engine driven by
 * c/lib/text/glyphras.c) uses 16 (its SUB_SHIFT was 4), not 4, and it uses
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
 * SIZED FOR TWO WORKLOADS THIS ENGINE DID NOT SERVE WHEN THE NUMBERS WERE
 * CHOSEN: text-as-paths (now landed -- c/lib/text/glyphras.c) and SVG, both
 * scheduled in docs/superpowers/specs/2026-08-14-open-logit-2-design.md.
 * The old numbers (edges 1536, active 512) were sized for the corner quadrants
 * and simple shapes phase 1 actually draws (<= 64 segments) and silently broke
 * on anything bigger -- see gfx_raster.c's overflow handling for what "broke"
 * meant before this milestone.
 *
 * GFX_MAX_EDGES 8192 matched c/kernel/gui/raster.c's MAXEDGE exactly, which was
 * not a coincidence: that was the kernel's OWN glyph rasterizer, sized for a
 * CJK glyph at a large pixel size (Noto Sans SC, up to ~2.2 MB of glyf data),
 * and proven sufficient in production -- zh.wikipedia.org had rendered through
 * it since M14. Text has since moved onto this engine and raster.c is deleted,
 * so this number is no longer an inheritance from a neighbour: it is the cap
 * that workload actually runs against. Measured (make test-glyph-agree): the
 * densest glyph in any shipped font, at the largest size the kernel's glyph
 * cache can serve, flattens to 1,079 points -- one edge each -- against this
 * 8,192. It is also generous for SVG path data: a hand-drawn icon runs to
 * dozens of segments, not thousands.
 *
 * GFX_MAX_ACTIVE 1024 matched the same rasterizer's MAXCROSS (crossings lived
 * per sub-scanline there, since it had no persistent active-edge list and
 * rescanned every edge each sub-scanline instead) -- the same proven-sufficient
 * number, reused rather than picked fresh, for the same reason. Concurrently
 * active edges can only be a fraction of total edges in real geometry (nothing
 * plausible has 1024 contours simultaneously open at one scanline), so this is
 * generous headroom over GFX_MAX_EDGES 8192, not a matching pair by
 * necessity.
 *
 * GFX_MAX_W is untouched at 4096: it bounds gfx_row's width, which is desktop
 * pixels wide (today's largest bench case is 2560), not glyph- or path-segment
 * related, and 4096 already clears every real display width with margin.
 * Correction (2026-09-22): 5K exceeds that old display-width claim. Keep this
 * coverage-row cap for a single vector fill; packed display targets instead
 * use OL_DISPLAY_MAX_SIZE and dynamic effect scratch. Enlarging a framebuffer
 * no longer silently enlarges these static raster arrays or truncates a fill.
 *
 * COST: this is kernel .bss (see the note at the top of this file). The five
 * rasterizer statics (g_edge/g_order/g_act/g_cross/g_row in gfx_raster.c) were
 * 43,008 B at the old caps; at these caps they are 194,560 B -- see the
 * per-array arithmetic in gfx_raster.c's file comment. A sixth has since
 * joined them, g_acc (8,192 B), the exact coverage accumulator that the glyph
 * migration proved was necessary; its own comment in gfx_raster.c carries the
 * measurement. Plus the (unchanged)
 * mask cache (82,944 B) and corner-tile scratch (4,128 B), the engine's total
 * static footprint goes from about 130,080 B to about 281,632 B, a delta of
 * 151,552 B (~148 KiB). Raising these is a real cost paid by every kernel
 * boot, not a free number -- it earns its place only because it is the exact
 * number this OS already runs a CJK-glyph workload through today. */
#define GFX_MAX_EDGES  8192    /* edges considered in one fill              */
#define GFX_MAX_ACTIVE 1024    /* edges crossing one scanline               */
#define GFX_MAX_W      4096    /* device pixels across one fill             */

/* Fill rules: nonzero (winding) or even-odd. */
enum { GFX_NONZERO = 0, GFX_EVENODD = 1 };

/* --------------------------------------------------------------- surface --
 * An RGBA8 tile with STRAIGHT (non-premultiplied) alpha, matching what
 * SYS_GUI_BLIT consumes -- so a rendered surface goes to the screen with one
 * blit and no conversion pass. The engine never reads the window back (ring 3
 * cannot), so a surface is always something the caller owns and hands over. */
struct gfx_surface { unsigned char *px; int w, h, stride; };

void gfx_surface_init(struct gfx_surface *s, unsigned char *px, int w, int h, int stride);
void gfx_surface_clear(struct gfx_surface *s);

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
/* 2026-09-13: independent device workspaces for openlogit.h. Storage is
 * caller-owned, aligned to 8 bytes and never shared during rendering.
 * Legacy fill/mask/clip entry points retain their serial-call contract. */
unsigned long gfx_raster_workspace_size(void);
int gfx_fill_with_workspace(void *storage, unsigned long bytes,
                           struct gfx_surface *dst, const struct gfx_path *p, int rule,
                           const struct gfx_paint *paint, const struct gfx_rect *clip, int subs);

/* Composite one straight-RGBA source pixel over one straight-RGBA destination
 * pixel, src-over, with `cov` (0..255) modulating the source alpha. */
void gfx_over(unsigned char *dst, int sr, int sg, int sb, int sa, int cov);

/* PATH CLIPPING. The rasterizer already produces coverage; clipping a fill to
 * a path is multiplying two coverages together. The multiply point is the
 * row_fn seam gfx_raster.c already has (the `fn` callback declared at gfx_raster.c:256,
 * called once per finished row at gfx_raster.c:344) -- but raster() is NOT REENTRANT (its
 * edge/active/crossing tables are file statics (legacy_workspaces, gfx_raster.c:92), so a clip
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

#endif /* GFX_RASTER_H */
