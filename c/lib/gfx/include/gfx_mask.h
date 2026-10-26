#ifndef GFX_MASK_H
#define GFX_MASK_H

/* gfx_mask.h -- the coverage-mask cache, the corner and shadow generators, and the
 * mask-to-RGBA expansion. */

/* ------------------------------------------------------------ mask cache --
 * Keyed by exact DEVICE geometry, so a frame full of controls that share a
 * radius rasterizes one quadrant and blits it 4n times.
 *
 * GFX_MASK_SHADOW is not a rasterization at all -- it is an analytic distance
 * ramp, because a shadow's alpha is a blur and not a coverage. It lives here
 * because it shares the cache, not because it shares the scanline loop. */
enum { GFX_MASK_FILL = 1, GFX_MASK_RING = 2, GFX_MASK_SHADOW = 3 };
#define GFX_MASK_MAX 72        /* largest cached tile edge, device px */

const unsigned char *gfx_mask_corner(int kind, int w, int h, int param);
void gfx_mask_stats(int *hits, int *misses);      /* for the cost tests */
/* Oversize refusals since boot. A NULL from gfx_mask_corner is a REFUSAL the
 * caller must handle, not a cache miss -- and it used to be invisible even to
 * gfx_mask_stats (mmiss++ sat after the early return). Counted separately so a
 * harness can assert "no shape silently degraded this frame". */
int  gfx_mask_refused(void);

/* Corner generators, callable directly when the caller does its own caching. */
void gfx_corner_fill  (unsigned char *m, int w, int h);
void gfx_corner_ring  (unsigned char *m, int w, int h, int t);
void gfx_corner_shadow(unsigned char *m, int w, int h, int r);
/* The shadow's alpha profile: 255 at the caster's edge, quadratic ease-out to 0
 * at `blur`. Exposed because a shadow's four corners are tiles but its four
 * EDGES are 1-pixel strips the compositor replicates, and both have to fall off
 * by the same curve or the corners show as seams. */
int gfx_shadow_falloff(long d256, long blur256);

/* Expand a coverage tile into straight RGBA for a blit, optionally mirrored --
 * which is how ONE rasterized quadrant serves all four corners. */
void gfx_mask_to_rgba(unsigned char *dst, const unsigned char *cov, int w, int h,
                      unsigned color, int alpha, int flipx, int flipy);

#endif /* GFX_MASK_H */
