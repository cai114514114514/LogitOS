#ifndef LOGIT_GLYPHRAS_H
#define LOGIT_GLYPHRAS_H

#include <stdint.h>
#include "ttf.h"

/* Glyph rasterization -- the three entry points every piece of type in this
 * machine goes through, implemented in glyphras.c ON TOP OF Open Logit
 * (c/lib/gfx).
 *
 * WHY THIS FILE EXISTS AT ALL. It used to be c/kernel/gui/raster.c: 285 lines
 * that were a COMPLETE second scanline coverage rasterizer -- its own 8.8 fixed
 * point, its own edge table at MAXEDGE 8192, its own heapsort of the crossings,
 * its own 16 sub-scanlines. Open Logit exists to be the one rasterizer in this
 * tree, so a second one drawing every label, menu, title and CJK page was the
 * largest thing still contradicting it. glyphras.c is what is left after that
 * rasterizer is deleted: a CONVERTER from a font outline to a gfx_path, and one
 * call to gfx_fill_mask_subs(). It rasterizes nothing itself.
 *
 * The declarations live here, in c/lib/text beside the font parsers that feed
 * them, rather than in c/kernel/gui/text.h where text_raster's used to: the
 * implementation is ring-3-clean library code (no kheap, no vfs, no fb) and the
 * kernel's text engine is only one of its callers -- the host font suites are
 * the others. c/kernel/gui/text.h and c/lib/text/fontcolor.h both include this
 * so their existing callers are unchanged. */

/* Rasterize glyph `gid` of `f` at `px` pixels into an 8-bit coverage bitmap.
 * On success: *w,*h = bitmap size; *ox = x offset from the pen origin to the
 * bitmap's left; *oy = pixels from the baseline up to the bitmap's top (so the
 * bitmap's top-left sits at (pen_x + *ox, baseline_y - *oy)). `cov` must hold
 * >= (*w)*(*h) bytes (covcap). Returns 0 ok, -1 on error / cov too small. A
 * blank glyph is a SUCCESS with *w == *h == 0. */
int text_raster(const struct ttf_font *f, int gid, int px,
                uint8_t *cov, int covcap, int *w, int *h, int *ox, int *oy);

/* --- the two colour-layer entry points ---
 * These exist for painting several outlines into ONE bitmap box: text_raster
 * picks a box from the glyph it is drawing, which would differ per layer and
 * leave a COLR composite's layers misaligned. So the caller fixes the box once
 * with text_raster_extent and every layer paints into it with text_raster_at. */

/* The box text_raster would choose for `gid` at `px`, without painting: *ox_i is
 * the x offset from the pen origin to the bitmap's left column, *top_i the
 * pixels from the baseline up to its top row. Returns 0, -1 for a blank glyph. */
int text_raster_extent(const struct ttf_font *f, int gid, int px,
                       int *ox_i, int *top_i, int *w, int *h);

/* Paint `gid` into a w*h coverage bitmap placed at (ox_i, top_i). The caller
 * composites the result with the layer's colour and zeroes/reuses `cov` between
 * layers as it likes -- this overwrites every byte it is given. */
int text_raster_at(const struct ttf_font *f, int gid, int px, int ox_i, int top_i,
                   uint8_t *cov, int w, int h);

/* Highest point count any single glyph has needed since boot, and the number of
 * outlines refused because they did not fit. Exposed for ONE reason: the point
 * budget below is a fixed static (this code has no allocator), a glyph that
 * overflows it is a glyph that does not draw, and "no label lost a character"
 * is a claim that has to be measurable rather than asserted. The agreement test
 * prints both over every glyph of every shipped font. */
int glyphras_peak_points(void);
int glyphras_refused(void);

#endif /* LOGIT_GLYPHRAS_H */
