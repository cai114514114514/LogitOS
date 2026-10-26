#ifndef GFX_PAINT_INTERNAL_H
#define GFX_PAINT_INTERNAL_H
/* Private to c/lib/gfx/raster: the span helpers gfx_paint.c dispatches to, and the
 * image sampler. Not installed, and no public header includes it. */
#include "gfx_raster.h"

void gfx_paint_row(const struct gfx_paint *p, struct gfx_surface *dst, int y,
                   int x0, int x1, const unsigned char *cov, int cov_ox);
void gfx_gradient_linear_row(const struct gfx_paint *p, unsigned char *drow, int y,
                             int x0, int x1, const unsigned char *cov, int cov_ox);
void gfx_gradient_radial_row(const struct gfx_paint *p, unsigned char *drow, int y,
                             int x0, int x1, const unsigned char *cov, int cov_ox);
void gfx_image_row(const struct gfx_paint *p, unsigned char *drow, int y,
                   int x0, int x1, const unsigned char *cov, int cov_ox);
void gfx_sample_image(const struct gfx_paint *p, int x, int y,
                      int *r, int *g, int *b, int *a);

#endif /* GFX_PAINT_INTERNAL_H */
