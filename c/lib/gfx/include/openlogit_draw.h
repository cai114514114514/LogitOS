#ifndef OPENLOGIT_DRAW_H
#define OPENLOGIT_DRAW_H
#include "openlogit.h"
/* Synchronous borrowed-target API for owners of existing pixel storage
 * (glyph caches, decoded SVG, Canvas, the compositor). No full-window copy
 * per primitive: the owner batches writes and publishes its damage once.
 * Like geometry builders, these raster entry points return 1 on success,
 * 0 on refusal, preserving the precision pipeline's existing error contract.
 * RGBA surfaces contain straight alpha. Coverage output is A8, never RGBA.
 * Calls without an explicit workspace are serialized by their owner; use
 * distinct devices/ol_submit for concurrent rendering and frame rollback. */
int ol_raster_fill(struct gfx_surface *, const struct gfx_path *, int,
                   const struct gfx_paint *, const struct gfx_rect *);
int ol_raster_fill_subs(struct gfx_surface *, const struct gfx_path *, int,
                        const struct gfx_paint *, const struct gfx_rect *, int samples);
int ol_raster_fill_clipped(struct gfx_surface *, const struct gfx_path *, int,
                           const struct gfx_paint *, const struct gfx_rect *, int samples,
                           const struct gfx_clip_mask *);
int ol_raster_fill_workspace(void *, unsigned long bytes, struct gfx_surface *,
                             const struct gfx_path *, int, const struct gfx_paint *,
                             const struct gfx_rect *, int samples);
int ol_raster_mask(const struct gfx_path *, int, unsigned char *, int w, int h, int ox, int oy);
int ol_raster_mask_subs(const struct gfx_path *, int, unsigned char *, int w, int h, int ox,
                        int oy, int samples);
int ol_raster_mask_clipped(const struct gfx_path *, int, unsigned char *, int w, int h,
                           int ox, int oy, int samples, const struct gfx_clip_mask *);
void ol_raster_clear(struct gfx_surface *);
/* Counts SDK raster invocations in this address space, for runtime path
 * evidence. Neither a submitted-frame counter nor a presentation counter. */
uint64_t ol_raster_calls(void);
#endif
