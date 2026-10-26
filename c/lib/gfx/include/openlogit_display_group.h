#ifndef OPENLOGIT_DISPLAY_GROUP_H
#define OPENLOGIT_DISPLAY_GROUP_H
#include "openlogit_display.h"

/* SNAPSHOT GROUPS on an opaque packed ol_display target: an opacity group, a
 * rounded-rect clip, or both, without an alpha target or a second rasterizer.
 *
 * THE IDENTITY. For an opaque destination and children that composite
 * src-over (fills, glyphs, gradients, blits), drawing the children into a
 * transparent layer and compositing that layer with weight w gives exactly
 *     dst = snap + (drawn - snap) * w
 * where snap is the destination before the group and drawn is the destination
 * after drawing the children straight onto it. w = opacity/255 * coverage of
 * the rounded rect at that pixel. So begin() saves the pixels whose w can be
 * below 1, the children draw normally (clipped to the rect), and end() blends.
 *
 *   ol_display_group_bytes  snapshot size for this group. With opacity 255
 *       only the four corner tiles (radius[k] x radius[k] pixels each) can have
 *       w < 1, so that is all it covers; with any other opacity it is the full
 *       w x h rect. 0 means no snapshot is needed (opacity 255, square corners:
 *       the group is a plain rect clip), and also an invalid argument.
 *   ol_display_group_begin  validates, saves the snapshot pixels, intersects
 *       ctx->target's clip with the rect and remembers the previous clip in g.
 *   ol_display_group_end    blends dst = snap + (drawn - snap) * w with exact
 *       rounding, then restores the previous clip. Outside the rounded shape
 *       but inside the rect w = 0, so the snapshot comes back bit-exactly;
 *       where w = 1 the drawn pixel is left as it is. Bits outside the three
 *       channels are taken from the drawn pixel, so an unchanged pixel is
 *       bit-exact and a group never invents a backdrop marker. Pixels outside
 *       the rect (or outside the clip in force at begin) are never written.
 *
 * GEOMETRY. (x,y,w,h) is in target device pixels. radius[] is top-left,
 * top-right, bottom-right, bottom-left (the GUI ABI v2 order), device pixels;
 * NULL means square. Each radius is clamped to min(w,h)/2, the same clamp
 * ol_display_round_rect applies. Corner coverage is the engine's own corner
 * tile, gfx_mask_corner(GFX_MASK_FILL, r, r), for r <= GFX_MASK_MAX -- so a
 * group clips content to exactly the shape ol_display_round_rect paints for a
 * frame of the same radius -- and an analytic quarter-circle coverage
 * (8 sub-rows, exact horizontal coverage, mirrored across the diagonal)
 * computed in display_group.c for larger radii, where gfx_mask_corner refuses.
 * opacity is 0..255.
 *
 * NESTING. Groups nest LIFO: end the innermost first. Each level has its own
 * struct ol_display_group and its own snap buffer, both owned by the caller
 * and live until end(). end() returns OL_ARGUMENT when the target's clip is
 * not the one its begin() installed (an inner group left open, or somebody
 * changed the clip in between); it still blends and restores, so the target
 * is left consistent, but the result may be wrong and the caller has a bug.
 *
 * SCRATCH. snap == NULL uses display_scratch(ctx, OL_DS_GROUP): the
 * per-display (so, in the kernel, per drawing context) scratch slot. That is
 * ONLY for a group that is not nested inside another scratch group and whose
 * begin and end run in one call scope, as the WM compositor path does: a
 * second scratch group would overwrite (or, if it grows the slot, free) the
 * first one's snapshot. end() detects the freed case (the slot no longer holds
 * the buffer begin took) and returns OL_ARGUMENT without blending; it cannot
 * detect an overwrite. Anything that nests, or spans syscalls, passes its own
 * buffer of at least ol_display_group_bytes() bytes.
 *
 * STATUS. OL_OK; OL_LIMIT when snap_bytes is smaller than
 * ol_display_group_bytes() or the scratch slot cannot grow (nothing is changed
 * in either case); OL_ARGUMENT for everything else (NULL ctx or group, an
 * unbound target, w or h outside 0..OL_DISPLAY_MAX_SIZE, a negative radius,
 * opacity outside 0..255, end() of a group that is not active).
 *
 * KERNEL CALLERS. In the kernel the ol_display is fb.c's per-context
 * g_fbctx[gfx_ctx()].disp, and fb_sdk() REBINDS its target -- clip included --
 * from the current struct surface's clip_* fields whenever the fb context is
 * dirty or the target changes (fb.c fb_sdk). So a group whose begin and end
 * span several fb_* calls or syscalls must set its clip through the surface
 * (fb_clip_push / fb_clip_pop, fb-clip-stack), not rely on the clip begin()
 * writes into ctx->target: that clip is lost on the next rebind and children
 * would draw outside the rect. Conversely a surface clip changed between
 * begin and end leaves later draws with a stale display clip, and end()
 * reports it (OL_ARGUMENT, above). The target must not be rebound to a
 * different pixel buffer between begin and end.
 *
 * LIMITATION. Exact only for src-over children. A child that READS the
 * destination with anything other than src-over -- glass, blur, a backdrop
 * material -- samples the pre-fade backdrop: glass inside a fading group shows
 * the content under the group at full strength and then fades as a whole,
 * which is not what an isolated layer would give.
 *
 * STATE. No file-scope state: the group lives in the caller's struct and
 * buffer or in the display's own scratch slot, so the kernel's per-context
 * displays never share anything (-DGFX_NCTX). Freestanding: no libc, no float. */

/* g->flags */
#define OL_DISPLAY_GROUP_ACTIVE  1u   /* begin succeeded, end not yet called */
#define OL_DISPLAY_GROUP_CORNERS 2u   /* snap holds the four corner tiles only */
#define OL_DISPLAY_GROUP_SCRATCH 4u   /* snap is display_scratch(OL_DS_GROUP) */

struct ol_display_group {
    int x, y, w, h;
    int radius[4];
    int opacity;
    int prev_clip_on, prev_clx0, prev_cly0, prev_clx1, prev_cly1;
    uint32_t *snap;
    unsigned long snap_bytes;
    unsigned flags;
};

unsigned long ol_display_group_bytes(int w, int h, const int radius[4], int opacity);
int ol_display_group_begin(struct ol_display *ctx, struct ol_display_group *g,
                           int x, int y, int w, int h, const int radius[4],
                           int opacity, uint32_t *snap, unsigned long snap_bytes);
int ol_display_group_end(struct ol_display *ctx, struct ol_display_group *g);

#endif
