#ifndef OPENLOGIT_DISPLAY_BLIT_H
#define OPENLOGIT_DISPLAY_BLIT_H
#include "openlogit_display.h"

/* Filtered RGBA blit and glyph blit with alpha, onto a native ol_display.
 *
 * Sources are straight-alpha RGBA bytes (r,g,b,a), sstride BYTES per row, so
 * a sub-image or a padded row is addressed without a copy. The destination
 * rectangle is in 24.8 target pixels (callers convert points with their own
 * scale, e.g. 125% => dw24 = pt * 256 * 5 / 4); fractional edges are drawn
 * with area coverage. All temporaries live in display_scratch(OL_DS_BLIT).
 *
 *   NEAREST   point sample.
 *   BILINEAR  4 taps, premultiplied, 16.16 weights.
 *   BOX       exact area average on every axis whose downscale ratio is > 2;
 *             an axis at or below that ratio uses the BILINEAR taps.
 *
 * opacity 0..255 multiplies the source alpha and the edge coverage. The
 * target clip is honoured. Returns OL_OK, OL_ARGUMENT (bad rect/stride) or
 * OL_LIMIT (scratch allocation failed; nothing drawn). */
enum ol_filter { OL_FILTER_NEAREST, OL_FILTER_BILINEAR, OL_FILTER_BOX };

int ol_display_blit_rgba_ex(struct ol_display *ctx, const uint8_t *src,
                            int sw, int sh, int sstride,
                            int src_x, int src_y, int src_w, int src_h,
                            int dx24, int dy24, int dw24, int dh24,
                            int opacity, enum ol_filter filter);

/* Like ol_display_blit_glyph, with a row stride and a global alpha 0..255.
 * alpha 255 with stride == w is bit-identical to ol_display_blit_glyph. */
void ol_display_blit_glyph_a(struct ol_display *ctx, int x, int y,
                             const uint8_t *cov, int w, int h, int stride,
                             uint32_t color, int alpha);
#endif
