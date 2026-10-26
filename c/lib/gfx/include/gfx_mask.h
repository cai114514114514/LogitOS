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
#define GFX_MASK_MAX 72        /* largest tile edge held in the fixed slots, device px */
#define GFX_MASK_BIG 512       /* largest tile edge served from the byte arena */

const unsigned char *gfx_mask_corner(int kind, int w, int h, int param);
void gfx_mask_stats(int *hits, int *misses);      /* for the cost tests */
/* Oversize refusals since boot. A NULL from gfx_mask_corner is a REFUSAL the
 * caller must handle, not a cache miss -- and it used to be invisible even to
 * gfx_mask_stats (mmiss++ sat after the early return). Counted separately so a
 * harness can assert "no shape silently degraded this frame". */
int  gfx_mask_refused(void);

/* Big tiles (GFX_MASK_MAX < edge <= GFX_MASK_BIG) come from a per-context byte
 * arena the owner of the context supplies: it applies to the CURRENT gfx_ctx(),
 * so a band thread calls it after it has registered itself. LRU eviction keeps
 * the live bytes within `bytes`. With no arena (or a tile larger than it) the
 * request is refused (NULL) and counted by gfx_mask_refused(). Setting the
 * arena drops that context's entries. gfx_mask_flush_all invalidates every
 * context's big tiles (each drops them at its next request); call it when the
 * device scale changes. */
void gfx_mask_set_arena(void *buf, unsigned long bytes);
void gfx_mask_flush_all(void);
unsigned long gfx_mask_arena_used(void);   /* live bytes, current context */
int  gfx_mask_big_refused(void);           /* refusals of big tiles only */

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
