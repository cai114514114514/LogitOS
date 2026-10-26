/* Host test for the large-radius corner-mask arena (gfx-mask-large-radius).
 * Built with -DGFX_NCTX=2 and no fb.c, so it defines gfx_ctx_hook itself.
 * Prints "FAIL <name>" per failed check and exits 1 if any failed. The control
 * builds with -DGFX_MASK_LEGACY_CAP (refuse past GFX_MASK_MAX), which must fail
 * 'FAIL r180'. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "gfx_ctx.h"

int (*gfx_ctx_hook)(void);
static int g_ctx;
static int hook(void) { return g_ctx; }

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); bad = 1; } } while (0)

/* Independent reference: 8x8 samples per pixel against the exact quarter disc
 * centred on the tile's bottom-right corner (the test-aui-mask oracle). */
static double ref_fill(int i, int j, double w, double h)
{
    int in = 0;
    for (int sy = 0; sy < 8; sy++)
        for (int sx = 0; sx < 8; sx++) {
            double x = i + (sx + 0.5) / 8, y = j + (sy + 0.5) / 8;
            double dx = (w - x) / w, dy = (h - y) / h;
            if (dx * dx + dy * dy <= 1.0) in++;
        }
    return in / 64.0;
}
static double ref_ring(int i, int j, double w, double h, double t)
{
    int in = 0;
    for (int sy = 0; sy < 8; sy++)
        for (int sx = 0; sx < 8; sx++) {
            double x = i + (sx + 0.5) / 8, y = j + (sy + 0.5) / 8;
            double ox = (w - x) / w, oy = (h - y) / h;
            if (ox * ox + oy * oy > 1.0) continue;
            double ix = (w - x) / (w - t), iy = (h - y) / (h - t);
            if (x >= t && y >= t && ix * ix + iy * iy <= 1.0) continue;
            in++;
        }
    return in / 64.0;
}
static double ref_shadow(int i, int j, double w, double h, double r)
{
    double dx = w - (i + 0.5), dy = h - (j + 0.5);
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    double d = sqrt(dx * dx + dy * dy) - r, blur = w - r;
    if (d <= 0) return 1.0;
    if (d >= blur) return 0.0;
    double u = 1.0 - d / blur;
    return u * u;
}

static int worst(const unsigned char *m, int w, int h, int kind, int param)
{
    int mx = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            double e = kind == GFX_MASK_FILL ? ref_fill(i, j, w, h)
                     : kind == GFX_MASK_RING ? ref_ring(i, j, w, h, param)
                     : ref_shadow(i, j, w, h, param);
            int d = abs((int)(e * 255.0 + 0.5) - (int)m[j * w + i]);
            if (d > mx) mx = d;
        }
    return mx;
}

/* The rasteriser samples 8 sub-scanlines, so against the exact quarter disc a
 * fill/ring tile is off by up to 1/8 coverage on the shallow rows near the
 * tangent (32/255 at r72, the pre-existing small path; 36 at r400). RAST_TOL
 * bounds that inherent error; it does not hide a truncated path, which blows
 * far past it. The cache itself must add NOTHING: a served tile is compared
 * bit for bit with the uncached generator. */
#define RAST_TOL 40
#define RAST_TOL_512 50   /* 46 measured at r512: the 1/8 row quantisation plus slope */
static int same_as_direct(const unsigned char *m, int kind, int w, int h, int param)
{
    unsigned char *d = malloc((size_t)w * h);
    if (kind == GFX_MASK_FILL) gfx_corner_fill(d, w, h);
    else if (kind == GFX_MASK_RING) gfx_corner_ring(d, w, h, param);
    else gfx_corner_shadow(d, w, h, param);
    int eq = memcmp(d, m, (size_t)w * h) == 0;
    free(d);
    return eq;
}

static unsigned char *arena_new(unsigned long n) { return malloc(n); }

static void check_oracle(void)
{
    static const int radii[] = { 72, 180, 400 };
    for (int k = 0; k < 3; k++) {
        int r = radii[k];
        const unsigned char *m = gfx_mask_corner(GFX_MASK_FILL, r, r, 0);
        CHECK(m != NULL, "r%d", r);
        if (m) { int e = worst(m, r, r, GFX_MASK_FILL, 0);
                 CHECK(e <= RAST_TOL, "r%d fill err %d", r, e); CHECK(same_as_direct(m, GFX_MASK_FILL, r, r, 0), "r%d fill != direct", r); printf("  r%d fill max err %d\n", r, e); }
        int t = r / 8;
        m = gfx_mask_corner(GFX_MASK_RING, r, r, t);
        CHECK(m != NULL, "r%d ring", r);
        if (m) { int e = worst(m, r, r, GFX_MASK_RING, t);
                 CHECK(e <= RAST_TOL, "r%d ring err %d", r, e); CHECK(same_as_direct(m, GFX_MASK_RING, r, r, t), "r%d ring != direct", r); printf("  r%d ring max err %d\n", r, e); }
        int blur = r / 6 + 4, w = r + blur;
        if (w > GFX_MASK_BIG) w = GFX_MASK_BIG;
        m = gfx_mask_corner(GFX_MASK_SHADOW, w, w, w - blur);
        CHECK(m != NULL, "r%d shadow", r);
        if (m) { int e = worst(m, w, w, GFX_MASK_SHADOW, w - blur);
                 CHECK(e <= 2, "r%d shadow err %d", r, e); CHECK(same_as_direct(m, GFX_MASK_SHADOW, w, w, w - blur), "r%d shadow != direct", r); printf("  r%d shadow max err %d\n", r, e); }
    }
    /* non-square and the edge of the range */
    const unsigned char *m = gfx_mask_corner(GFX_MASK_FILL, 200, 120, 0);
    CHECK(m && worst(m, 200, 120, GFX_MASK_FILL, 0) <= RAST_TOL && same_as_direct(m, GFX_MASK_FILL, 200, 120, 0), "r200x120");
    m = gfx_mask_corner(GFX_MASK_FILL, GFX_MASK_BIG, GFX_MASK_BIG, 0);
    CHECK(m && worst(m, GFX_MASK_BIG, GFX_MASK_BIG, GFX_MASK_FILL, 0) <= RAST_TOL_512, "r512");
    m = gfx_mask_corner(GFX_MASK_RING, GFX_MASK_BIG, GFX_MASK_BIG, 40);
    CHECK(m && worst(m, GFX_MASK_BIG, GFX_MASK_BIG, GFX_MASK_RING, 40) <= RAST_TOL_512, "r512 ring");
    CHECK(gfx_mask_corner(GFX_MASK_FILL, GFX_MASK_BIG + 1, 100, 0) == NULL, "r513 refused");
}

static void check_eviction(void)
{
    unsigned long budget = 500000;                 /* fits 3 x 400^2 = 480000 */
    unsigned char *a = arena_new(budget);
    gfx_mask_set_arena(a, budget);
    int h0, m0, h1, m1;
    static const int sz[] = { 400, 380, 360, 300, 250, 400, 200, 180, 400 };
    for (unsigned n = 0; n < sizeof sz / sizeof *sz; n++) {
        const unsigned char *m = gfx_mask_corner(GFX_MASK_FILL, sz[n], sz[n], 0);
        CHECK(m != NULL, "evict req %u", n);
        CHECK(m >= a && m + (long)sz[n] * sz[n] <= a + budget, "evict inside arena %u", n);
        CHECK(gfx_mask_arena_used() <= budget, "evict budget %u used=%lu", n, gfx_mask_arena_used());
        if (m) CHECK(same_as_direct(m, GFX_MASK_FILL, sz[n], sz[n], 0), "evict content %u", n);
    }
    gfx_mask_stats(&h0, &m0);
    gfx_mask_corner(GFX_MASK_FILL, 400, 400, 0);   /* most recent: hit */
    gfx_mask_stats(&h1, &m1);
    CHECK(h1 == h0 + 1 && m1 == m0, "evict lru keeps newest");
    gfx_mask_corner(GFX_MASK_FILL, 380, 380, 0);   /* long evicted: miss */
    gfx_mask_stats(&h1, &m1);
    CHECK(m1 == m0 + 1, "evict oldest dropped");
    /* one tile larger than the whole budget */
    unsigned char *s = arena_new(100000);
    gfx_mask_set_arena(s, 100000);
    int r0 = gfx_mask_big_refused();
    CHECK(gfx_mask_corner(GFX_MASK_FILL, 400, 400, 0) == NULL, "small arena refuses");
    CHECK(gfx_mask_big_refused() == r0 + 1, "small arena counted");
    gfx_mask_set_arena(NULL, 0);
    free(a); free(s);
}

static void check_noarena(void)
{
    gfx_mask_set_arena(NULL, 0);
    int rf = gfx_mask_refused(), br = gfx_mask_big_refused();
    CHECK(gfx_mask_corner(GFX_MASK_FILL, 180, 180, 0) == NULL, "noarena null");
    CHECK(gfx_mask_refused() == rf + 1 && gfx_mask_big_refused() == br + 1, "noarena counted");
    CHECK(gfx_mask_corner(GFX_MASK_FILL, 72, 72, 0) != NULL, "noarena small ok");
}

static void check_interleave(void)
{
    unsigned long n0 = 700000, n1 = 200000;
    unsigned char *a0 = arena_new(n0), *a1 = arena_new(n1);
    g_ctx = 0; gfx_mask_set_arena(a0, n0);
    g_ctx = 1; gfx_mask_set_arena(a1, n1);
    g_ctx = 0;
    const unsigned char *p0 = gfx_mask_corner(GFX_MASK_FILL, 300, 300, 0);
    CHECK(p0 >= a0 && p0 < a0 + n0, "ctx0 arena");
    if (!p0) { g_ctx = 1; gfx_mask_set_arena(NULL, 0); g_ctx = 0; gfx_mask_set_arena(NULL, 0); free(a0); free(a1); return; }
    unsigned long used0 = gfx_mask_arena_used();
    unsigned sum = 0; for (int i = 0; i < 300 * 300; i++) sum += p0[i];
    for (int n = 0; n < 12; n++) {                 /* ctx1 thrashes its small arena */
        g_ctx = 1;
        int s = 150 + 20 * (n % 4);
        const unsigned char *p1 = gfx_mask_corner(GFX_MASK_RING, s, s, 10);
        CHECK(p1 >= a1 && p1 + (long)s * s <= a1 + n1, "ctx1 arena %d", n);
        CHECK(gfx_mask_arena_used() <= n1, "ctx1 budget %d", n);
        g_ctx = 0;
        const unsigned char *q = gfx_mask_corner(GFX_MASK_FILL, 300, 300, 0);
        CHECK(q == p0, "ctx0 hit stable %d", n);
    }
    unsigned sum2 = 0; for (int i = 0; i < 300 * 300; i++) sum2 += p0[i];
    CHECK(sum == sum2 && gfx_mask_arena_used() == used0, "ctx0 untouched by ctx1");
    g_ctx = 1; gfx_mask_set_arena(NULL, 0);
    g_ctx = 0; gfx_mask_set_arena(NULL, 0);
    free(a0); free(a1);
}

static void check_flush(void)
{
    unsigned long n = 400000;
    unsigned char *a = arena_new(n);
    g_ctx = 0; gfx_mask_set_arena(a, n);
    g_ctx = 1; gfx_mask_set_arena(a, 0);           /* ctx1: none */
    g_ctx = 0;
    const unsigned char *m = gfx_mask_corner(GFX_MASK_FILL, 180, 180, 0);
    CHECK(m != NULL, "flush precondition tile");
    if (!m) { gfx_mask_set_arena(NULL, 0); free(a); return; }
    unsigned char copy[180 * 180]; memcpy(copy, m, sizeof copy);
    int h0, m0, h1, m1;
    gfx_mask_stats(&h0, &m0);
    gfx_mask_corner(GFX_MASK_FILL, 180, 180, 0);
    gfx_mask_stats(&h1, &m1);
    CHECK(h1 == h0 + 1 && m1 == m0, "flush precondition hit");
    gfx_mask_flush_all();
    gfx_mask_stats(&h0, &m0);
    m = gfx_mask_corner(GFX_MASK_FILL, 180, 180, 0);
    gfx_mask_stats(&h1, &m1);
    CHECK(m && m1 == m0 + 1, "flush forces regeneration");
    CHECK(m && memcmp(copy, m, sizeof copy) == 0, "flush regenerates identical tile");
    CHECK(gfx_mask_arena_used() == 180 * 180, "flush dropped old entries");
    g_ctx = 0; gfx_mask_set_arena(NULL, 0);
    free(a);
}

int main(void)
{
    gfx_ctx_hook = hook;
    unsigned long n = 1UL << 20;
    unsigned char *a = arena_new(n);
    gfx_mask_set_arena(a, n);
    check_oracle();
    gfx_mask_set_arena(NULL, 0);
    check_eviction();
    check_noarena();
    check_interleave();
    check_flush();
    free(a);
    if (bad) { printf("gfx_mask_big: FAILED\n"); return 1; }
    printf("gfx_mask_big: ok\n");
    return 0;
}
