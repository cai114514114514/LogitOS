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
/* Status variant of ol_raster_fill. OL_OK when the fill ran; OL_ARGUMENT for a
 * null target, paint or path, a path whose overflow latched while it was built,
 * or any other refusal of the legacy call; OL_LIMIT when the path is wider than
 * GFX_MAX_W (4096) device pixels or holds more than GFX_MAX_EDGES (8192) points
 * (each point adds at most one edge, so the point count bounds the edge count).
 * The legacy 1/0 functions are unchanged. */
int ol_raster_fill_st(struct gfx_surface *, const struct gfx_path *, int,
                      const struct gfx_paint *, const struct gfx_rect *);
void ol_raster_clear(struct gfx_surface *);
/* Counts SDK raster invocations in this address space, for runtime path
 * evidence. Neither a submitted-frame counter nor a presentation counter. */
uint64_t ol_raster_calls(void);
#endif
