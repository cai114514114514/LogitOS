#ifndef GFX_CTX_H
#define GFX_CTX_H

/* WHICH COPY OF THE ENGINE'S SCRATCH A CALLER GETS.
 *
 * c/lib/gfx keeps a few file-scope work areas -- the corner-mask cache and its
 * path scratch (gfx_mask.c), the rasteriser's workspace (gfx_raster.c) --
 * because one caller at a time was always true: a ring-3 app draws from one
 * thread, and the kernel compositor ran on one core. The compositor now splits
 * a frame into horizontal bands and renders them on several cores at once
 * (c/kernel/gui/wm.c, wm_bands.h), and two cores sharing a mask cache evict
 * each other's tiles mid-blit.
 *
 * So each of those areas is an array of GFX_NCTX copies, and gfx_ctx() picks
 * one. A ring-3 build leaves GFX_NCTX at 1 and gfx_ctx() is the constant 0 --
 * nothing changes for an app, not even a load. The kernel builds with
 * -DGFX_NCTX=<n> and installs gfx_ctx_hook = the current core's index; a core
 * whose index is >= GFX_NCTX never renders a band (the compositor checks), so
 * the index is always in range where it matters and clamps to 0 elsewhere. */

#ifndef GFX_NCTX
#define GFX_NCTX 1
#endif

#if GFX_NCTX > 1
extern int (*gfx_ctx_hook)(void);
static inline int gfx_ctx(void)
{
    int i = gfx_ctx_hook ? gfx_ctx_hook() : 0;
    return (i >= 0 && i < GFX_NCTX) ? i : 0;
}
#else
static inline int gfx_ctx(void) { return 0; }
#endif

#endif
