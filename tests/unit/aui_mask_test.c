/* Host unit test for aui's coverage rasterizer.
 *
 * The rasterizer is the reason the toolkit's shapes are not staircases, and it
 * is pure integer arithmetic over a byte buffer -- so it can be checked exactly,
 * on the host, in milliseconds, instead of by booting a machine and squinting at
 * a screenshot. tests/qmp/qmp_gallery.py still proves it reaches the screen;
 * this proves the shape is right, which is the half that is cheap to get wrong.
 *
 * It includes aui.c directly to reach the static rasterizer entry points. The
 * syscall inline asm in logit.h assembles fine on an x86-64 host and none of it
 * is ever executed here.
 *
 * Build: make test-aui-mask
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "aui.c"

static int fails;

static void ck(int cond, const char *what, const char *detail)
{
    printf("%-4s %s%s%s\n", cond ? "ok" : "FAIL", what,
           detail && *detail ? "  " : "", detail ? detail : "");
    if (!cond) fails++;
}

static void dump(const unsigned char *m, int w, int h, const char *title)
{
    printf("     %s (%dx%d)\n", title, w, h);
    for (int j = 0; j < h; j++) {
        printf("     ");
        for (int i = 0; i < w; i++) {
            int v = m[j * w + i];
            putchar(v == 0 ? '.' : (v >= 250 ? '#' : '0' + v * 9 / 255));
        }
        putchar('\n');
    }
}

/* Reference coverage by brute force: 16x16 samples per pixel against the exact
 * analytic shape. Slow, obvious, and independent of the code under test -- which
 * is the entire point of a reference. */
static double ref_fill(int i, int j, double w, double h)
{
    int in = 0;
    for (int sy = 0; sy < 16; sy++)
        for (int sx = 0; sx < 16; sx++) {
            double x = i + (sx + 0.5) / 16.0, y = j + (sy + 0.5) / 16.0;
            double dx = (w - x) / w, dy = (h - y) / h;
            if (dx * dx + dy * dy <= 1.0) in++;
        }
    return in / 256.0;
}

static double ref_stroke(int i, int j, double w, double h, double t)
{
    int in = 0;
    for (int sy = 0; sy < 16; sy++)
        for (int sx = 0; sx < 16; sx++) {
            double x = i + (sx + 0.5) / 16.0, y = j + (sy + 0.5) / 16.0;
            double ox = (w - x) / w, oy = (h - y) / h;
            if (ox * ox + oy * oy > 1.0) continue;              /* outside the outer */
            /* The inner rounded rect is inset by t, so its corner arc has the
             * SAME centre (w,h) and radii (w-t, h-t). Writing the centre as
             * (w-t, h-t) instead -- the intuitive-looking mistake -- describes a
             * ring that thins toward the diagonal, and this reference would then
             * "prove" a correct rasterizer wrong. */
            double ix = (w - x) / (w - t), iy = (h - y) / (h - t);
            if (x >= t && y >= t && ix * ix + iy * iy <= 1.0) continue;  /* inside the inner */
            in++;
        }
    return in / 256.0;
}

static double worst_err(const unsigned char *m, int w, int h, int stroke, int t)
{
    double worst = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            double want = stroke ? ref_stroke(i, j, w, h, t) : ref_fill(i, j, w, h);
            double got = m[j * w + i] / 255.0;
            double e = got - want; if (e < 0) e = -e;
            if (e > worst) worst = e;
        }
    return worst;
}

/* How much of a shape's boundary is PARTIALLY covered. A hard-edged rasterizer
 * scores 0 here by construction; that number is what the whole file is for. */
static int partials(const unsigned char *m, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++) if (m[i] > 8 && m[i] < 247) k++;
    return k;
}

int main(void)
{
    static unsigned char m[MASK_MAX * MASK_MAX];
    char d[128];

    printf("=== aui coverage rasterizer ===\n");

    /* ---- filled corner ---- */
    for (int r = 4; r <= 32; r += 4) {
        raster_fill_corner(m, r, r);
        double e = worst_err(m, r, r, 0, 0);
        snprintf(d, sizeof d, "r=%d worst pixel error %.3f", r, e);
        ck(e < 0.10, "filled corner matches a 16x supersampled reference", d);
    }
    raster_fill_corner(m, 24, 24);
    snprintf(d, sizeof d, "%d of %d pixels", partials(m, 24 * 24), 24 * 24);
    ck(partials(m, 24 * 24) >= 20, "filled corner has real partial coverage", d);
    /* Every row of a quadrant must end fully inside the shape, and the coverage
     * along a row must never decrease -- a rasterizer that gets the fractional
     * end wrong shows up here as a dip. */
    int mono = 1, ends = 1;
    for (int j = 0; j < 24; j++) {
        for (int i = 1; i < 24; i++)
            if (m[j * 24 + i] + 2 < m[j * 24 + i - 1]) mono = 0;
        if (m[j * 24 + 23] < 250) ends = 0;
    }
    ck(mono, "coverage is monotonic across each row", "");
    ck(ends, "each row reaches full coverage at the straight edge", "");
    dump(m, 24, 24, "fill corner r=24");

    /* ---- elliptical corner (the scaled case: cw != ch) ---- */
    raster_fill_corner(m, 20, 14);
    {
        double e = worst_err(m, 20, 14, 0, 0);
        snprintf(d, sizeof d, "worst %.3f", e);
        ck(e < 0.10, "a non-square corner is an ellipse quadrant, not a squashed circle", d);
    }

    /* ---- stroked corner ---- */
    for (int r = 8; r <= 28; r += 4) {
        for (int t = 1; t <= 4; t++) {
            raster_stroke_corner(m, r, r, t);
            double e = worst_err(m, r, r, 1, t);
            if (e >= 0.14) {
                snprintf(d, sizeof d, "r=%d t=%d worst %.3f", r, t, e);
                ck(0, "stroked corner matches the reference ring", d);
            }
        }
    }
    ck(1, "stroked corner matches the reference ring at every r,t tried", "");

    raster_stroke_corner(m, 24, 24, 3);
    dump(m, 24, 24, "stroke corner r=24 t=3");
    /* The ring must be CONTINUOUS: every row of the tile that the arc passes
     * through has to carry ink. A ring that pinches to nothing mid-arc is the
     * bug this test was written for -- it renders as a rounded box whose corners
     * fade out before they reach the straight edges. */
    int empty_rows = 0;
    for (int j = 0; j < 24; j++) {
        int sum = 0;
        for (int i = 0; i < 24; i++) sum += m[j * 24 + i];
        if (sum < 200) empty_rows++;
    }
    snprintf(d, sizeof d, "%d rows carry less than one pixel of ink", empty_rows);
    ck(empty_rows == 0, "the stroked arc is continuous down the whole tile", d);
    /* ...and it has to arrive at the straight edges at full thickness: the last
     * row of the tile abuts the vertical bar aui_stroke() draws. */
    int last = 0, first = 0;
    for (int i = 0; i < 24; i++) { last += m[23 * 24 + i]; first += m[0 * 24 + i]; }
    snprintf(d, sizeof d, "top row ink %d, bottom row ink %d", first, last);
    ck(first > 400 && last > 400, "the arc meets both straight edges at full weight", d);

    /* ---- shadow corner ---- */
    raster_shadow_corner(m, 24, 24, 8);
    dump(m, 24, 24, "shadow corner r=8 blur=16");
    ck(m[23 * 24 + 23] > 240, "a shadow is opaque at the caster's edge", "");
    ck(m[0] < 12, "and transparent at the far corner", "");
    {
        int falling = 1;
        for (int k = 1; k < 24; k++)
            if (m[(23 - k) * 24 + 23] > m[(24 - k) * 24 + 23]) falling = 0;
        ck(falling, "shadow alpha decreases monotonically away from the edge", "");
    }
    ck(partials(m, 24 * 24) > 200, "the shadow is a ramp, not a hard silhouette", "");

    /* ---- the cache is a cache ---- */
    const unsigned char *a = mask_get(MK_FILL, 12, 12, 0);
    const unsigned char *b = mask_get(MK_FILL, 12, 12, 0);
    ck(a == b, "identical geometry hits the mask cache", "");
    ck(mask_get(MK_FILL, 12, 12, 0) != mask_get(MK_STROKE, 12, 12, 1),
       "kind is part of the cache key", "");
    ck(mask_get(MK_FILL, MASK_MAX + 1, 8, 0) == 0,
       "an oversized tile is refused rather than overflowing the pool", "");

    /* ---- aui_stroke past the ceiling: corners must not vanish ----
     *
     * THE BUG THIS MILESTONE FIXED. aui_stroke() had NO check on mask_get()'s
     * return at all -- unlike round_impl just above it in this file, which at
     * least fell back to a square. A radius past GFX_MASK_MAX (72 device px)
     * went straight into blit_mask with a NULL cov, which silently no-ops,
     * while the four straight edges aui_stroke draws either side of the
     * corners kept going regardless. The picture that reached the screen was
     * not visibly broken -- it was a DIFFERENT, complete-looking shape (a
     * plain square outline where a rounded one was asked for), which is
     * worse than an obviously wrong one because nothing about it says "this
     * is a fallback". See gfx_mask.c's contract comment on gfx_mask_corner
     * for why that specific failure (geometry silently dropped, not
     * downgraded) is the one case this file's rule never allows.
     *
     * This test cannot call aui_stroke() itself -- this file's own comment at
     * the top is explicit that no syscall in this TU is ever allowed to
     * actually fire, and aui_stroke ends in gui_blit/gui_rect, real int 0x80
     * on this host. What it CAN call, with no syscall anywhere near it, is
     * corner_mask() -- the exact function aui.c's fix made aui_stroke (and
     * round_impl, and aui_vgrad_round, and aui_shadow_ex) go through for
     * their corner source, cache or uncached -- with the same MK_STROKE kind
     * and an oversized radius, and ask the question that matters: does a
     * corner come back AT ALL, and is it whole. */
    {
        int big = MASK_MAX + 40;             /* well past the 72px cache ceiling */
        unsigned char *rbuf; long rcap;
        const unsigned char *bm = corner_mask(MK_STROKE, big, big, 3, &rbuf, &rcap);
        ck(bm != 0, "aui_stroke's corner source survives a radius past the cache ceiling",
           "corner_mask() falls back to the uncached ring instead of handing back NULL");
        if (bm) {
            int empty_rows = 0;
            for (int j = 0; j < big; j++) {
                int sum = 0;
                for (int i = 0; i < big; i++) sum += bm[j * big + i];
                if (sum < 200) empty_rows++;
            }
            snprintf(d, sizeof d, "%d of %d rows carry less than one pixel of ink", empty_rows, big);
            ck(empty_rows == 0,
               "...and the ring it hands back is CONTINUOUS -- corners not missing, not pinched", d);
        }
    }

    printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
