#ifndef OPENLOGIT_SW_H
#define OPENLOGIT_SW_H
/* PRIVATE software implementation. Only SDK implementation files include
 * this header. It never calls compatibility drawing functions: otherwise
 * gfx_fill -> SDK -> gfx_fill would recurse. Geometry builders are shared. */
#include "gfx.h"
struct ol_layer_options;
int ol_sw_raster_fill(void *workspace, struct gfx_surface *, const struct gfx_path *, int rule,
                      const struct gfx_paint *, const struct gfx_rect *, int samples,
                      const struct gfx_clip_mask *);
int ol_sw_raster_mask(void *workspace, const struct gfx_path *, int rule, unsigned char *cov,
                      int w, int h, int ox, int oy, int samples, const struct gfx_clip_mask *);
void ol_sw_surface_clear(struct gfx_surface *);
void ol_sw_layer(struct gfx_surface *, const struct gfx_surface *, struct gfx_rect,
                  const struct ol_layer_options *, void *scratch);
struct gfx_rect ol_sw_effect_bounds(struct gfx_rect, int radius, int width, int height);
unsigned long ol_sw_backdrop_size(struct gfx_rect, int radius, int width, int height);
void ol_sw_backdrop(struct gfx_surface *, struct gfx_rect, int radius,
                     unsigned rgb, int opacity, void *scratch);
#endif
