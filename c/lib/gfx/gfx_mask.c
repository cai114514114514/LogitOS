/* Open Logit -- the device-geometry mask cache and the corner tiles.
 *
 * WHY MASKS AND NOT FULL SURFACES. A rounded rect is three opaque bands plus
 * four r x r corner tiles -- so drawing one costs O(r^2), not O(w*h), and the
 * bands go out as ordinary opaque rect calls on the compositor's fast path.
 * Only what CURVES is ever rasterized. Cache those four tiles by their exact
 * DEVICE geometry and the second control with the same radius rasterizes
 * nothing at all: a screen of buttons is one 8x8 quadrant, blitted 4n times.
 *
 * DEVICE geometry, emphatically. The tile is generated at the size the blit's
 * destination occupies in device pixels, so the compositor's nearest-neighbour
 * rescale is the identity. Generate at 1x and let it stretch to a 2x window and
 * you have a blurry mask of a sharp shape -- worse than the staircase it was
 * meant to replace.
 *
 * The fill and ring tiles are ordinary paths through the one rasterizer. The
 * SHADOW tile is not: a shadow's alpha is a blur, not a coverage, so it is an
 * analytic distance ramp. It lives here because it shares the cache, not
 * because it shares the scanline loop. */
#include "gfx.h"

#define NMASK 16     /* Sized by what one FRAME asks for. A page mixing control
                      * radii, a stroked outline, badges and four elevations of
                      * shadow wants a dozen distinct geometries; with eight
                      * slots the round-robin evicted a mask the same frame then
                      * asked for again, so every frame re-rasterized every
                      * corner. Sixteen slots is 83 KB in an app that has
                      * megabytes. */

static unsigned char mdata[NMASK][GFX_MASK_MAX * GFX_MASK_MAX];
static int mkind[NMASK], mkw[NMASK], mkh[NMASK], mkp[NMASK];
static int mnext, mhit, mmiss;

/* Path scratch for the corner tiles. At the 1/64-pixel tolerance below a
 * 72-pixel quadrant flattens to about 64 segments, so two arcs fit inside 512
 * points with room to spare. */
static int cpt[512 * 2];
static int csub[8];

/* The quarter-ellipse corner region, in tile coordinates: the tile is [0,w] x
 * [0,h] and the arc is centred on the tile's BOTTOM-RIGHT corner (w,h) with
 * semi-axes (w,h). So the covered part is the arc, the right edge and the
 * bottom edge -- which is exactly the top-left corner of a rounded rect. */
static void arc_quadrant(struct gfx_path *p, int rx, int ry, int cx, int cy)
{
    /* From (cx-rx, cy) up to (cx, cy-ry): one cubic, kappa-approximated (the
     * same constant gfx_path_ellipse uses, worst radial error 2.7e-4 of r). */
    int kx = (int)((long long)rx * 36195 >> 16);
    int ky = (int)((long long)ry * 36195 >> 16);
    gfx_move_to(p, cx - rx, cy);
    gfx_cubic_to(p, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    gfx_line_to(p, cx, cy);
    gfx_close(p);
}

void gfx_corner_fill(unsigned char *m, int w, int h)
{
    gfx_zero(m, w * h);
    if (w <= 0 || h <= 0) return;
    struct gfx_path p;
    gfx_path_init(&p, cpt, 512, csub, 8);
    gfx_path_tolerance(&p, GFX_ONE / 64);
    arc_quadrant(&p, w * GFX_ONE, h * GFX_ONE, w * GFX_ONE, h * GFX_ONE);
    gfx_fill_mask(&p, GFX_NONZERO, m, w, h, 0, 0);
}

/* The same quadrant hollowed by `t` device pixels.
 *
 * THE TRAP, and it is on record because test-aui-mask caught it: the inner
 * ellipse shares the OUTER'S CENTRE and differs only in its radii (w-t, h-t).
 * Insetting the centre to (w-t, h-t) instead -- the mistake that looks right --
 * describes a ring that thins toward the diagonal and pinches to nothing before
 * it meets the straight edges, which renders as a rounded box whose corners
 * fade out. The inner arc's straight ends land at x=t and y=t, which is exactly
 * where the inner rounded rect's straight edges are, so the two abut. */
void gfx_corner_ring(unsigned char *m, int w, int h, int t)
{
    gfx_zero(m, w * h);
    if (w <= 0 || h <= 0 || t <= 0) return;
    struct gfx_path p;
    gfx_path_init(&p, cpt, 512, csub, 8);
    gfx_path_tolerance(&p, GFX_ONE / 64);
    arc_quadrant(&p, w * GFX_ONE, h * GFX_ONE, w * GFX_ONE, h * GFX_ONE);
    if (w - t > 0 && h - t > 0)
        arc_quadrant(&p, (w - t) * GFX_ONE, (h - t) * GFX_ONE, w * GFX_ONE, h * GFX_ONE);
    /* EVENODD, not nonzero: the inner region is wholly inside the outer, so the
     * difference is their symmetric difference and the two subpaths need not be
     * wound opposite ways for it to come out right. */
    gfx_fill_mask(&p, GFX_EVENODD, m, w, h, 0, 0);
}

int gfx_shadow_falloff(long d256, long blur256)
{
    if (d256 <= 0) return 255;
    if (blur256 <= 0 || d256 >= blur256) return 0;
    long t = d256 * 256 / blur256;              /* 0..256 */
    long u = 256 - t;
    return (int)(255 * u * u / 65536);          /* quadratic ease-out */
}

/* Blurred top-left corner of a drop shadow. The tile is (blur+r) square, the
 * caster's rounded corner is centred at the tile's bottom-right with radius
 * `r`, and alpha falls off over `blur` outside it. */
void gfx_corner_shadow(unsigned char *m, int w, int h, int r)
{
    long blur = (long)(w - r) * 256;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            long dx = (long)w * 256 - ((long)i * 256 + 128);
            long dy = (long)h * 256 - ((long)j * 256 + 128);
            if (dx < 0) dx = 0;
            if (dy < 0) dy = 0;
            long d = (long)gfx_isqrt((unsigned long long)(dx * dx + dy * dy)) - (long)r * 256;
            m[(long)j * w + i] = (unsigned char)gfx_shadow_falloff(d, blur);
        }
    }
}

const unsigned char *gfx_mask_corner(int kind, int w, int h, int param)
{
    if (w <= 0 || h <= 0 || w > GFX_MASK_MAX || h > GFX_MASK_MAX) return 0;
    for (int i = 0; i < NMASK; i++)
        if (mkind[i] == kind && mkw[i] == w && mkh[i] == h && mkp[i] == param) {
            mhit++;
            return mdata[i];
        }
    mmiss++;
    int s = mnext; mnext = (mnext + 1) % NMASK;
    mkind[s] = kind; mkw[s] = w; mkh[s] = h; mkp[s] = param;
    if (kind == GFX_MASK_FILL)      gfx_corner_fill(mdata[s], w, h);
    else if (kind == GFX_MASK_RING) gfx_corner_ring(mdata[s], w, h, param);
    else                            gfx_corner_shadow(mdata[s], w, h, param);
    return mdata[s];
}

void gfx_mask_stats(int *hits, int *misses)
{
    if (hits) *hits = mhit;
    if (misses) *misses = mmiss;
}

void gfx_mask_to_rgba(unsigned char *dst, const unsigned char *cov, int w, int h,
                      unsigned color, int alpha, int flipx, int flipy)
{
    if (!dst || !cov || w <= 0 || h <= 0) return;
    int r = GFX_R(color), g = GFX_G(color), b = GFX_B(color);
    for (int j = 0; j < h; j++) {
        int sj = flipy ? h - 1 - j : j;
        unsigned char *d = dst + (long)j * w * 4;
        const unsigned char *s = cov + (long)sj * w;
        for (int i = 0; i < w; i++) {
            int si = flipx ? w - 1 - i : i;
            int a = s[si];
            if (alpha < 255) a = a * alpha / 255;
            d[i * 4 + 0] = (unsigned char)r;
            d[i * 4 + 1] = (unsigned char)g;
            d[i * 4 + 2] = (unsigned char)b;
            d[i * 4 + 3] = (unsigned char)a;
        }
    }
}
