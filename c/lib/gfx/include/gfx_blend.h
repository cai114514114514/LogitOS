#ifndef GFX_BLEND_H
#define GFX_BLEND_H

/* gfx_blend.h -- blend operators. Only src-over exists. Zero is src-over, so a
 * zero-filled gfx_paint composites exactly as it did before the op field existed. */

enum gfx_op { GFX_OP_SRC_OVER = 0 };

#endif /* GFX_BLEND_H */
