#ifndef GFX_BLEND_H
#define GFX_BLEND_H

/* gfx_blend.h -- blend modes and Porter-Duff operators. Zero is src-over, so a
 * zero-filled gfx_paint composites exactly as it did before the op field existed.
 *
 * The maths follows the W3C Compositing and Blending spec on STRAIGHT RGBA:
 *     blend modes   aR = aS + aB(1-aS)
 *                   CR = [aS(1-aB) Cs + aS aB B(Cb,Cs) + (1-aS) aB Cb] / aR
 *     Porter-Duff   aR = aS Fa + aB Fb,  CR = [aS Fa Cs + aB Fb Cb] / aR
 * Coverage scales the source alpha for src-over, PLUS_LIGHTER and the blend modes.
 * The Porter-Duff operators that act on the destination where the source is absent
 * (SRC, CLEAR, DST_IN, DST_OUT, SRC_IN, SRC_ATOP, XOR) blend the full-coverage result
 * with the untouched destination by coverage, so an anti-aliased edge of a SRC or
 * CLEAR fill fades instead of punching a hard hole. */
#include <stdint.h>

enum gfx_op {
    GFX_OP_SRC_OVER = 0,
    GFX_OP_SRC, GFX_OP_CLEAR, GFX_OP_DST_IN, GFX_OP_DST_OUT, GFX_OP_SRC_IN,
    GFX_OP_SRC_ATOP, GFX_OP_XOR, GFX_OP_PLUS_LIGHTER,
    GFX_OP_MULTIPLY, GFX_OP_SCREEN, GFX_OP_OVERLAY, GFX_OP_DARKEN, GFX_OP_LIGHTEN,
    GFX_OP_SOFT_LIGHT, GFX_OP_COLOR_DODGE,
    GFX_OP_COUNT
};

/* Composite one source pixel (colour 0xRRGGBB, alpha src_a 0..255, coverage cov
 * 0..255) onto the straight-RGBA pixel dst_rgba[0..3] with operator `op`. An op
 * outside the enum composites as src-over. */
void gfx_compose(enum gfx_op op, uint8_t *dst_rgba, uint32_t src_rgb, int src_a, int cov);

#endif /* GFX_BLEND_H */
