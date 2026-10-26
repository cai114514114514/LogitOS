/* Glass blur tiers (THIN / REGULAR / THICK / ULTRA).
 *
 * Built with -DGFX_NCTX=2 and without fb.c, so this file defines
 * gfx_ctx_hook itself. Exit status is non-zero on any failure; every failing
 * check prints "FAIL <name>".
 *
 * The impulse response is measured with a stripe of width sigma rather than a
 * one-pixel impulse: the blur is 8-bit per pass, and a 255 impulse spread over
 * a sigma-20 gaussian peaks at ~5 levels, so a one-pixel probe would measure
 * quantisation, not the kernel. The stripe is the impulse convolved with a box
 * and is compared with the same stripe convolved with the double gaussian. */
#include "openlogit_display.h"
#include "openlogit_glass_material.h"
#include "glass_internal.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int (*gfx_ctx_hook)(void);
static int current_ctx;
static int hook(void) { return current_ctx; }

static int checks, failures;
static int scratch_allocations;

static void *counted_alloc(unsigned long bytes)
{
    scratch_allocations++;
    return malloc(bytes);
}

static void check(int condition, const char *name)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static const char *const tier_name[4] = {"REGULAR", "THIN", "THICK", "ULTRA"};
static const double tier_sigma[4] = {0, 3, 12, 20};

static void bind(struct ol_display *d, struct ol_pixel_target *t, uint32_t *px,
                 int w, int h)
{
    memset(d, 0, sizeof *d);
    memset(t, 0, sizeof *t);
    d->allocate = counted_alloc;
    d->release = free;
    t->px = px;
    t->w = w;
    t->h = h;
    if (ol_display_bind(d, t, 16, 8, 0) != OL_OK) {
        fprintf(stderr, "cannot bind display\n");
        exit(2);
    }
}

/* Run only the blur stage of a tier over a whole-target sample box. */
static void blur_only(uint32_t *px, int w, int h, int blur)
{
    struct ol_display d;
    struct ol_pixel_target t;
    struct ol_glass_material mat = {.blur = (uint8_t)blur};
    struct glass_panel panel = {0};
    bind(&d, &t, px, w, h);
    d.glass_buf = malloc((size_t)w * h * sizeof(uint32_t));
    d.glass_buf_n = w * h;
    panel.display = &d;
    panel.sample_width = w;
    panel.sample_height = h;
    panel.mat = &mat;
    glass_blur(&panel);
    memcpy(px, d.glass_buf, (size_t)w * h * sizeof(uint32_t));
    free(d.glass_buf);
}

static void test_gaussian(void)
{
    enum { H = 6, W = 400 };
    static uint32_t px[H * W];
    for (int tier = OL_GLASS_BLUR_THIN; tier <= OL_GLASS_BLUR_ULTRA; tier++) {
        double sigma = tier_sigma[tier];
        int stripe = (int)sigma, left = W / 2 - stripe / 2;
        char name[64];
        memset(px, 0, sizeof px);
        for (int y = 0; y < H; y++)
            for (int x = left; x < left + stripe; x++) px[y * W + x] = 0xFFFFFF;
        blur_only(px, W, H, tier);

        /* Expected: the stripe convolved with a unit-sum gaussian. */
        double expect_sum = 0, err_sum = 0;
        int reach = 0;
        int reach_limit = glass_blur_support(&(struct ol_glass_material){.blur = (uint8_t)tier});
        for (int x = 0; x < W; x++) {
            double e = 0;
            for (int s = left; s < left + stripe; s++) {
                double d = x - s;
                e += exp(-d * d / (2 * sigma * sigma));
            }
            e *= 255.0 / (sigma * sqrt(2 * M_PI));
            double got = px[(H / 2) * W + x] & 255;
            expect_sum += e;
            err_sum += fabs(got - e);
            if (got > 0) {
                int dist = x < left ? left - x : (x >= left + stripe ? x - (left + stripe - 1) : 0);
                if (dist > reach) reach = dist;
            }
            /* all three channels and all rows must agree */
            check(px[x] == px[(H - 1) * W + x] &&
                  ((px[(H / 2) * W + x] >> 8) & 255) == (unsigned)got, "channels and rows agree");
        }
        double l1 = err_sum / expect_sum;
        printf("%s sigma=%.0f stripe=%d L1=%.4f reach=%d support=%d\n",
               tier_name[tier], sigma, stripe, l1, reach, reach_limit);
        snprintf(name, sizeof name, "gaussian %s", tier_name[tier]);
        check(l1 <= 0.06, name);
        snprintf(name, sizeof name, "reach within support %s", tier_name[tier]);
        check(reach <= reach_limit, name);
    }
}

static void test_flat(void)
{
    static const uint8_t levels[] = {0, 1, 37, 128, 200, 254, 255};
    enum { W = 150, H = 97 };  /* smaller than ULTRA's support on one axis */
    static uint32_t px[W * H];
    for (int tier = 0; tier < 4; tier++) {
        int worst = 0;
        for (unsigned l = 0; l < sizeof levels; l++) {
            uint32_t c = (uint32_t)levels[l] << 16 | (uint32_t)(255 - levels[l]) << 8 | levels[l] / 2;
            for (int i = 0; i < W * H; i++) px[i] = c;
            blur_only(px, W, H, tier);
            for (int i = 0; i < W * H; i++) {
                int d0 = abs((int)(px[i] >> 16 & 255) - levels[l]);
                int d1 = abs((int)(px[i] >> 8 & 255) - (255 - levels[l]));
                int d2 = abs((int)(px[i] & 255) - levels[l] / 2);
                if (d0 > worst) worst = d0;
                if (d1 > worst) worst = d1;
                if (d2 > worst) worst = d2;
            }
        }
        char name[64];
        snprintf(name, sizeof name, "flat field stays flat %s", tier_name[tier]);
        printf("flat %s worst deviation %d\n", tier_name[tier], worst);
        check(worst <= 1, name);
    }
}

static uint32_t lcg = 12345;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static void pattern(uint32_t *px, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v = rnd();
            px[y * w + x] = (x / 7 + y / 5) & 1 ? (v & 0xFFFFFF) : 0x203040 + (v & 0x3F3F3F);
        }
}

enum { SW = 320, SH = 240 };

static void render_ex(uint32_t *px, int tier, int reduce, unsigned cut, int x, int y,
                      int w, int h, int radius, uint32_t tint, uint8_t opacity,
                      uint8_t rim)
{
    struct ol_display d;
    struct ol_pixel_target t;
    struct ol_glass_material mat = {.tint = tint, .opacity = opacity,
                                    .blur = (uint8_t)tier, .rim = rim};
    bind(&d, &t, px, SW, SH);
    d.reduce_transparency = reduce;
    ol_display_liquid_glass_ex(&d, x, y, w, h, radius, cut, &mat);
    check(d.error == 0, "ex render without error");
    ol_display_destroy(&d);
}

static void render_legacy(uint32_t *px, int reduce, unsigned cut, int x, int y,
                          int w, int h, int radius, uint32_t tint, uint8_t opacity)
{
    struct ol_display d;
    struct ol_pixel_target t;
    bind(&d, &t, px, SW, SH);
    d.reduce_transparency = reduce;
    ol_display_liquid_glass_cut(&d, x, y, w, h, radius, tint >> 16 & 255,
                                tint >> 8 & 255, tint & 255, opacity, cut);
    check(d.error == 0, "legacy render without error");
    ol_display_destroy(&d);
}

static uint32_t base[SW * SH], a[SW * SH], b[SW * SH];

static void test_regular_identity(void)
{
    static const struct { int x, y, w, h, r; unsigned cut; uint32_t tint; uint8_t op; } cases[] = {
        {40, 30, 200, 120, 24, 0, 0xE0E8F0, 120},
        {-20, -10, 140, 90, 18, 0, 0x102030, 200},
        {250, 180, 120, 100, 30, 0, 0xFFFFFF, 60},
        {100, 60, 90, 50, 0, 0, 0x808080, 255},
        {60, 40, 160, 80, 20, 3, 0x404040, 90},
        {30, 30, 100, 100, 50, 0, 0x336699, 0},  /* Clear */
    };
    pattern(base, SW, SH);
    int same = 1;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        memcpy(a, base, sizeof base);
        memcpy(b, base, sizeof base);
        render_legacy(a, 0, cases[i].cut, cases[i].x, cases[i].y, cases[i].w, cases[i].h,
                      cases[i].r, cases[i].tint, cases[i].op);
        render_ex(b, OL_GLASS_BLUR_REGULAR, 0, cases[i].cut, cases[i].x, cases[i].y,
                  cases[i].w, cases[i].h, cases[i].r, cases[i].tint, cases[i].op,
                  OL_GLASS_RIM_DIRECTIONAL);
        if (memcmp(a, b, sizeof a)) same = 0;
        check(memcmp(a, base, sizeof a) != 0, "legacy render changes the backdrop");
        /* SYMMETRIC is accepted and renders as DIRECTIONAL for now. */
        memcpy(b, base, sizeof base);
        render_ex(b, OL_GLASS_BLUR_REGULAR, 0, cases[i].cut, cases[i].x, cases[i].y,
                  cases[i].w, cases[i].h, cases[i].r, cases[i].tint, cases[i].op,
                  OL_GLASS_RIM_SYMMETRIC);
        if (memcmp(a, b, sizeof a)) same = 0;
    }
    check(same, "REGULAR ex bit-identical to legacy");
    check(ol_display_glass_sample_halo_ex(200, 120, 24, 0) == ol_display_glass_sample_halo(200, 120, 24),
          "halo_ex(NULL) equals legacy halo");
    struct ol_glass_material reg = {0};
    check(ol_display_glass_sample_halo_ex(200, 120, 24, &reg) == ol_display_glass_sample_halo(200, 120, 24),
          "halo_ex(REGULAR) equals legacy halo");
}

static void test_tiers_differ(void)
{
    static uint32_t out[4][SW * SH];
    pattern(base, SW, SH);
    for (int tier = 0; tier < 4; tier++) {
        memcpy(out[tier], base, sizeof base);
        render_ex(out[tier], tier, 0, 0, 60, 50, 200, 120, 24, 0x8090A0, 100, 0);
    }
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) {
            char name[64];
            snprintf(name, sizeof name, "tiers differ %s %s", tier_name[i], tier_name[j]);
            check(memcmp(out[i], out[j], sizeof out[i]) != 0, name);
        }
}

static void test_halo(void)
{
    int prev = 0;
    for (int tier = 0; tier < 4; tier++) {
        struct ol_glass_material m = {.blur = (uint8_t)tier};
        int h = ol_display_glass_sample_halo_ex(200, 120, 24, &m);
        int need = glass_blur_support(&m);
        char name[64];
        snprintf(name, sizeof name, "halo covers kernel support %s", tier_name[tier]);
        check(h > need, name);
        snprintf(name, sizeof name, "halo grows with tier %s", tier_name[tier]);
        check(tier == 0 || h > prev, name);
        prev = h;
        check(ol_display_glass_sample_halo_ex(0, 120, 24, &m) == 0, "halo_ex of empty panel is 0");
    }

    /* End to end: the panel must not depend on any pixel beyond its halo. */
    for (int tier = 0; tier < 4; tier++) {
        struct ol_glass_material m = {.blur = (uint8_t)tier};
        int px0 = 120, py0 = 90, pw = 80, ph = 60, r = 16;
        int halo = ol_display_glass_sample_halo_ex(pw, ph, r, &m);
        pattern(base, SW, SH);
        memcpy(a, base, sizeof base);
        memcpy(b, base, sizeof base);
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++)
                if (x < px0 - halo || x >= px0 + pw + halo || y < py0 - halo || y >= py0 + ph + halo)
                    b[y * SW + x] = ~base[y * SW + x] & 0xFFFFFF;
        render_ex(a, tier, 0, 0, px0, py0, pw, ph, r, 0x8090A0, 100, 0);
        render_ex(b, tier, 0, 0, px0, py0, pw, ph, r, 0x8090A0, 100, 0);
        int ok = 1;
        for (int y = py0; y < py0 + ph; y++)
            for (int x = px0; x < px0 + pw; x++)
                if (a[y * SW + x] != b[y * SW + x]) ok = 0;
        char name[64];
        snprintf(name, sizeof name, "panel independent of pixels beyond halo %s", tier_name[tier]);
        check(ok, name);
    }
}

static void test_reduce_transparency(void)
{
    pattern(base, SW, SH);
    memcpy(a, base, sizeof base);
    render_legacy(a, 1, 0, 50, 40, 180, 110, 22, 0xC0D0E0, 140);
    for (int tier = 0; tier < 4; tier++) {
        char name[64];
        memcpy(b, base, sizeof base);
        int before = scratch_allocations;
        render_ex(b, tier, 1, 0, 50, 40, 180, 110, 22, 0xC0D0E0, 140, 0);
        snprintf(name, sizeof name, "reduce_transparency solid panel %s", tier_name[tier]);
        check(memcmp(a, b, sizeof a) == 0, name);
        snprintf(name, sizeof name, "reduce_transparency allocates no blur scratch %s", tier_name[tier]);
        check(scratch_allocations == before, name);
        /* and it differs from the translucent render */
        static uint32_t t[SW * SH];
        memcpy(t, base, sizeof base);
        render_ex(t, tier, 0, 0, 50, 40, 180, 110, 22, 0xC0D0E0, 140, 0);
        snprintf(name, sizeof name, "reduced differs from translucent %s", tier_name[tier]);
        check(memcmp(t, b, sizeof t) != 0, name);
    }
}

/* Two displays on different tiers, rendered alternately under two contexts,
 * must match their isolated renders. */
static void test_interleave(void)
{
    static uint32_t ia[SW * SH], ib[SW * SH], ra[SW * SH], rb[SW * SH];
    struct ol_display da, db;
    struct ol_pixel_target ta, tb;
    struct ol_glass_material ma = {.tint = 0x4060A0, .opacity = 90, .blur = OL_GLASS_BLUR_THICK};
    struct ol_glass_material mb = {.tint = 0xA06040, .opacity = 150, .blur = OL_GLASS_BLUR_THIN};
    pattern(base, SW, SH);
    memcpy(ra, base, sizeof base);
    memcpy(rb, base, sizeof base);
    current_ctx = 0;
    render_ex(ra, OL_GLASS_BLUR_THICK, 0, 0, 30, 30, 150, 100, 20, 0x4060A0, 90, 0);
    current_ctx = 1;
    render_ex(rb, OL_GLASS_BLUR_THIN, 0, 0, 90, 70, 190, 120, 14, 0xA06040, 150, 0);

    memcpy(ia, base, sizeof base);
    memcpy(ib, base, sizeof base);
    bind(&da, &ta, ia, SW, SH);
    bind(&db, &tb, ib, SW, SH);
    gfx_ctx_hook = hook;
    for (int step = 0; step < 2; step++) {
        /* second round re-renders over the first result on both sides */
        current_ctx = 0;
        ol_display_liquid_glass_ex(&da, 30, 30, 150, 100, 20, 0, &ma);
        current_ctx = 1;
        ol_display_liquid_glass_ex(&db, 90, 70, 190, 120, 14, 0, &mb);
        if (step == 0) {
            check(memcmp(ia, ra, sizeof ia) == 0, "interleaved ctx0 THICK matches isolated");
            check(memcmp(ib, rb, sizeof ib) == 0, "interleaved ctx1 THIN matches isolated");
        }
    }
    check(da.error == 0 && db.error == 0, "interleaved renders without error");
    ol_display_destroy(&da);
    ol_display_destroy(&db);
}

int main(void)
{
    gfx_ctx_hook = hook;
    test_gaussian();
    test_flat();
    test_regular_identity();
    test_tiers_differ();
    test_halo();
    test_reduce_transparency();
    test_interleave();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
