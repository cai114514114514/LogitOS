/* SPDX-License-Identifier: GPL-3.0-or-later */
/* icons.c with the real gfx engine: every id rasterises at the sizes the dock,
 * menus and 125%/150% scales produce, ink stays inside the box, mirrored and
 * rotated pairs agree, the 64-entry LRU keeps alternating sizes hot and is
 * bounded, and the kernel ICON_* ids equal the ring-3 GICON_* ids. Built with
 * -DGFX_NCTX=2 (no fb.c, so it defines gfx_ctx_hook). Prints "FAIL <name>" per
 * failing check and exits 1. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spinlock.h"
#include "icons.h"
#include "logit.h"          /* c/apps/logit.h with LOGIT_SYS_HOOK; ring-3 header, host-safe */

long ic_test_sys(long n, long a, long b, long c) { (void)n; (void)a; (void)b; (void)c; return 0; }

#define SAME(a, b) _Static_assert((a) == (b), #a " != " #b)
SAME(ICON_FOLDER, GICON_FOLDER);   SAME(ICON_DOC, GICON_DOC);
SAME(ICON_TERMINAL, GICON_TERMINAL); SAME(ICON_GRID, GICON_GRID);
SAME(ICON_GLOBE, GICON_GLOBE);     SAME(ICON_CODE, GICON_CODE);
SAME(ICON_CHART, GICON_CHART);     SAME(ICON_CLOCK, GICON_CLOCK);
SAME(ICON_IMAGE, GICON_IMAGE);
SAME(ICON_CHEVRON_L, GICON_CHEVRON_L); SAME(ICON_CHEVRON_R, GICON_CHEVRON_R);
SAME(ICON_CHEVRON_U, GICON_CHEVRON_U); SAME(ICON_CHEVRON_D, GICON_CHEVRON_D);
SAME(ICON_CLOSE, GICON_CLOSE);     SAME(ICON_PLUS, GICON_PLUS);
SAME(ICON_MINUS, GICON_MINUS);     SAME(ICON_CHECK, GICON_CHECK);
SAME(ICON_SEARCH, GICON_SEARCH);   SAME(ICON_SIDEBAR, GICON_SIDEBAR);
SAME(ICON_ELLIPSIS, GICON_ELLIPSIS); SAME(ICON_GEAR, GICON_GEAR);
SAME(ICON_SHARE, GICON_SHARE);     SAME(ICON_PLAY, GICON_PLAY);
SAME(ICON_PAUSE, GICON_PAUSE);     SAME(ICON_TRASH, GICON_TRASH);
SAME(ICON_ARROW_L, GICON_ARROW_L); SAME(ICON_ARROW_R, GICON_ARROW_R);
SAME(ICON_ARROW_U, GICON_ARROW_U); SAME(ICON_ARROW_D, GICON_ARROW_D);
SAME(ICON_INFO, GICON_INFO);       SAME(ICON_WARNING, GICON_WARNING);
SAME(ICON_DOWNLOAD, GICON_DOWNLOAD); SAME(ICON_REFRESH, GICON_REFRESH);
SAME(ICON_STAR, GICON_STAR);       SAME(ICON_LOCK, GICON_LOCK);
SAME(ICON_HOME, GICON_HOME);       SAME(ICON_LINK, GICON_LINK);
SAME(ICON_EYE, GICON_EYE);         SAME(ICON_BELL, GICON_BELL);
SAME(ICON_COUNT, GICON_COUNT);       /* set C follows set B (ui-wm-abi) */

/* --- stubs for what icons.c links against --- */
static int hook_fn(void) { return 0; }
int (*gfx_ctx_hook)(void) = hook_fn;
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void spin_lock(spinlock_t *l) { (void)l; }
void spin_unlock(spinlock_t *l) { (void)l; }

static uint8_t g_cov[512 * 512];
static int g_w, g_h;
void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color)
{ (void)x; (void)y; (void)color; g_w = w; g_h = h; memcpy(g_cov, cov, (size_t)w * h); }

static int bad;
#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s\n", name); bad = 1; } } while (0)

static long ink(void) { long s = 0; for (int i = 0; i < g_w * g_h; i++) s += g_cov[i]; return s; }
static int ring_clear(void)
{
    for (int i = 0; i < g_w; i++)
        if (g_cov[i] || g_cov[(g_h - 1) * g_w + i] || g_cov[i * g_w] || g_cov[i * g_w + g_w - 1]) return 0;
    return 1;
}

static void test_all_ids(void)
{
    static const int px[] = { 12, 16, 20, 32, 64, 15, 18, 24 };   /* last three: 125%/150% of 12/16 */
    int empty = 0, outside = 0, wrongsz = 0;
    for (int id = 0; id < ICON_COUNT; id++)
        for (unsigned k = 0; k < sizeof px / sizeof *px; k++) {
            g_w = g_h = 0;
            icon_draw(id, 0, 0, px[k], 0xFFFFFF);
            if (g_w != px[k] || g_h != px[k]) { wrongsz++; continue; }
            if (ink() == 0) empty++;
            /* the 11 legacy fills are free to touch the border; set A must not */
            if (id >= ICON_CHEVRON_L && !ring_clear()) outside++;
        }
    CHECK(!wrongsz, "size"); CHECK(!empty, "nonempty"); CHECK(!outside, "box");
}

/* worst per-pixel delta between a and the flip/rotation of b, both px x px */
static int delta(const uint8_t *a, const uint8_t *b, int px, int mode)
{
    int m = 0;
    for (int y = 0; y < px; y++) for (int x = 0; x < px; x++) {
        int sx = x, sy = y;
        if (mode == 0) sx = px - 1 - x;                    /* horizontal flip */
        else if (mode == 1) sy = px - 1 - y;               /* vertical flip */
        else { sx = y; sy = px - 1 - x; }                  /* rotate 90 */
        int d = abs((int)a[y * px + x] - (int)b[sy * px + sx]);
        if (d > m) m = d;
    }
    return m;
}

static void test_symmetry(void)
{
    static uint8_t A[64 * 64], B[64 * 64];
    int worst_m = 0, worst_plus = 0, worst_x = 0;
    static const int px[] = { 12, 16, 20, 32, 64, 15, 18, 24 };
    for (unsigned k = 0; k < sizeof px / sizeof *px; k++) {
        int n = px[k];
        icon_draw(ICON_CHEVRON_L, 0, 0, n, 1); memcpy(A, g_cov, (size_t)n * n);
        icon_draw(ICON_CHEVRON_R, 0, 0, n, 1); memcpy(B, g_cov, (size_t)n * n);
        int d = delta(A, B, n, 0); if (d > worst_m) worst_m = d;
        icon_draw(ICON_CHEVRON_U, 0, 0, n, 1); memcpy(A, g_cov, (size_t)n * n);
        icon_draw(ICON_CHEVRON_D, 0, 0, n, 1); memcpy(B, g_cov, (size_t)n * n);
        d = delta(A, B, n, 1); if (d > worst_m) worst_m = d;
        int ids[2] = { ICON_CLOSE, ICON_PLUS };
        for (int i = 0; i < 2; i++) {
            icon_draw(ids[i], 0, 0, n, 1); memcpy(A, g_cov, (size_t)n * n);
            d = delta(A, A, n, 2);
            if (i == 0) { if (d > worst_x) worst_x = d; } else if (d > worst_plus) worst_plus = d;
        }
    }
    printf("info mirror max delta %d/255, rotation max delta plus %d/255 close %d/255\n",
           worst_m, worst_plus, worst_x);
    CHECK(worst_m <= 1, "mirror");
    CHECK(worst_x <= 1, "rotate-close");
    /* PLUS has horizontal edges at fractional rows: x coverage is exact but y is
     * sampled in 64 sub-rows, so a horizontal bar and its rotated vertical twin
     * differ by up to one sub-row, 255/64 = 4 levels */
    CHECK(worst_plus <= 4, "rotate-plus");
}

/* wm.c's 512 px ceiling must not overflow the path scratch (a refused icon is
 * silently not drawn), and a stroke's overlapping subpaths must union (the
 * centre of PLUS is solid, not an even-odd hole). */
static void test_large_and_union(void)
{
    int refused = 0;
    for (int id = ICON_CHEVRON_L; id < ICON_COUNT; id++) {
        g_w = 0; icon_draw(id, 0, 0, 512, 1);
        if (g_w != 512 || ink() == 0) refused++;
    }
    CHECK(!refused, "large");
    icon_draw(ICON_PLUS, 0, 0, 32, 1);
    CHECK(g_cov[16 * 32 + 16] == 255 && g_cov[15 * 32 + 15] == 255, "union");
}

static void test_weight(void)
{
    icon_draw_w(ICON_PLUS, 0, 0, 32, 1, 1); long a = ink();
    icon_draw_w(ICON_PLUS, 0, 0, 32, 1, 2); long b = ink();
    icon_draw_w(ICON_PLUS, 0, 0, 32, 1, 3); long c = ink();
    icon_draw(ICON_PLUS, 0, 0, 32, 1);      long d = ink();
    CHECK(a < b && b < c, "weight"); CHECK(b == d, "weight-default");
    icon_draw_w(ICON_PLUS, 0, 0, 32, 1, 99); CHECK(ink() == c, "weight-clamp");
}

static void test_cache(void)
{
    unsigned h0, m0, e;
    icon_draw(ICON_GEAR, 0, 0, 16, 1); icon_draw(ICON_GEAR, 0, 0, 32, 1);   /* warm-up */
    icon_cache_stats(&h0, &m0, &e);
    for (int i = 0; i < 100; i++) icon_draw(ICON_GEAR, 0, 0, (i & 1) ? 32 : 16, 1);
    unsigned h1, m1;
    icon_cache_stats(&h1, &m1, &e);
    CHECK(h1 - h0 == 100 && m1 == m0, "hitrate");

    /* bound: 200 distinct keys */
    for (int i = 0; i < 200; i++) icon_draw(ICON_CHECK + (i % 4), 0, 0, 10 + i / 4, 1);
    icon_cache_stats(&h1, &m1, &e);
    CHECK(e <= 64, "bound");
#ifndef ICON_CACHE_SINGLE
    CHECK(e == 64, "bound-full");
    /* LRU order: key A touched last survives one more insertion */
    for (int i = 0; i < 64; i++) icon_draw(ICON_MINUS, 0, 0, 100 + i, 1);    /* flush */
    icon_draw(ICON_PLUS, 0, 0, 40, 1);                                       /* A */
    for (int i = 0; i < 63; i++) icon_draw(ICON_MINUS, 0, 0, 200 + i, 1);
    icon_draw(ICON_PLUS, 0, 0, 40, 1);                                       /* touch A */
    icon_draw(ICON_MINUS, 0, 0, 300, 1);                                     /* evicts oldest, not A */
    icon_cache_stats(&h0, &m0, &e);
    icon_draw(ICON_PLUS, 0, 0, 40, 1);
    icon_cache_stats(&h1, &m1, &e);
    CHECK(h1 == h0 + 1 && m1 == m0, "lru");
#endif
}

int main(void)
{
    test_all_ids();
    test_symmetry();
    test_large_and_union();
    test_weight();
    test_cache();
    if (!bad) printf("icons-symbols-a: ok\n");
    return bad;
}
