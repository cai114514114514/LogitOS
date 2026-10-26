#ifndef GFX_STROKE_H
#define GFX_STROKE_H

#include "gfx_path.h"

/* ============================================================================
 * PHASE 2 API -- STROKE. Implemented in c/lib/gfx/geometry/gfx_stroke.c. The other
 * phase-2 entry, path clipping, is declared in gfx_raster.h beside the fill it modifies.
 *
 * STROKE. Offset curves have no closed form, so a stroker either flattens then
 * offsets the polyline (joins and caps become explicit geometry) or fills the
 * stroke's outline as a path. This engine already flattens into device-space
 * polylines, so the second road is open: emit the offset outline as two
 * subpaths wound oppositely and fill it NONZERO -- gfx_stroke_path() below
 * turns a path into a path; it does not rasterize, so the caller fills the
 * result exactly as it would any other path. The work is joins (miter with a
 * limit, round, bevel), caps (butt/round/square), dashing, and the degenerate
 * cases -- a zero-length subpath with round caps is a dot, a cusp inside a
 * curve reverses the offset direction. The trap is already on record: aui's
 * stroked corner is an ELLIPSE SHARING THE OUTER'S CENTRE with radii reduced by
 * the width. Insetting the centre instead -- the mistake that looks right --
 * pinches the arc to nothing before it meets the straight edges, and
 * test-aui-mask caught exactly that; whatever replaces aui's stroker had
 * better fail the same way if it repeats the mistake. */
enum { GFX_CAP_BUTT = 0, GFX_CAP_ROUND = 1, GFX_CAP_SQUARE = 2 };
enum { GFX_JOIN_MITER = 0, GFX_JOIN_ROUND = 1, GFX_JOIN_BEVEL = 2 };

struct gfx_stroke {
    int width;              /* full stroke width (not half), device 24.8    */
    int cap;                /* GFX_CAP_*, applied to every open subpath end  */
    int join;                /* GFX_JOIN_*                                    */
    int miter_limit;         /* 16.16; miter length / width beyond which a
                              * GFX_JOIN_MITER corner falls back to bevel     */
    const int *dash;        /* device 24.8 on,off,on,off,... or NULL = solid */
    int ndash;               /* length of `dash`; must be even if non-NULL    */
    int dash_phase;          /* device 24.8 offset into the pattern at the
                              * start of each subpath                        */
};

/* Appends the stroke outline of every subpath in `src` to `out` (which the
 * caller has already gfx_path_init'd with its own storage -- a stroke costs
 * roughly 2x src's point count plus join/cap geometry, more for round joins).
 * 0 on refusal: `out` overflowed (out->overflow is latched the normal way),
 * `src` overflowed, or `stroke->width` <= 0. Refuses rather than emitting a
 * partial outline, same rule as everything else in this file. */
int gfx_stroke_path(struct gfx_path *out, const struct gfx_path *src,
                    const struct gfx_stroke *stroke);

#endif /* GFX_STROKE_H */
