#ifndef OPENLOGIT_DISPLAY_PATH_H
#define OPENLOGIT_DISPLAY_PATH_H
/* Fill and stroke gfx paths onto a packed ol_display target with any gfx_paint.
 *
 * The bridge between the gfx engine (straight-RGBA span painters) and the
 * window manager's packed BGRX/RGBX back buffer. Coverage comes from the
 * ordinary rasterizer into A8 band rows (display_scratch(OL_DS_PATH), so per
 * display and so per context); the paint span is evaluated in straight RGBA by
 * the gfx_paint span functions and composited src-over with coverage x opacity.
 *
 * Paint coordinates are DISPLAY pixels in 24.8 (the same space as the path).
 * `opacity` is 0..255 and multiplies coverage. paint->op is ignored here (all
 * paints composite src-over until gfx-blend-ops). The display scissor
 * (target.clip_*) is honoured. Targets wider than GFX_MAX_W are tiled.
 * Returns an ol_status: OL_OK, OL_ARGUMENT (null/bad input), OL_LIMIT (scratch
 * allocation failed, or the rasterizer/stroker refused: edge or point storage
 * exhausted, path->overflow). On failure nothing from the failing band on is drawn. */
#include "openlogit_display.h"
#include "gfx.h"

/* Composite a caller-made coverage mask, w x h at display (x,y), row pitch
 * `stride` bytes. */
int ol_display_fill_mask(struct ol_display *, int x, int y, const uint8_t *mask,
                         int w, int h, int stride,
                         const struct gfx_paint *, int opacity);
/* `subs`: sub-scanlines per pixel (<=0 selects GFX_SUBS). */
int ol_display_fill_path(struct ol_display *, const struct gfx_path *, int rule,
                         const struct gfx_paint *, int opacity, int subs);
/* Strokes `path` into `outline_scratch` (caller-initialised gfx_path storage,
 * reset here) and fills it non-zero. */
int ol_display_stroke_path(struct ol_display *, const struct gfx_path *,
                           const struct gfx_stroke *, struct gfx_path *outline_scratch,
                           const struct gfx_paint *, int opacity);
#endif
