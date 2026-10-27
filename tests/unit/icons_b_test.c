/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Icon set B (ICON_SHARE..ICON_BELL): real gfx engine, icons.c as shipped.
 * Checks: every new id is non-empty and keeps a clear border at 12/16/32/64 px
 * and the 125%/150% sizes; the arrow pairs are mirrors; each icon's ink area at
 * 32 px sits inside a band around its recorded value (a degenerate table, or one
 * whose stroke ops were dropped, falls outside); the 32 px coverage CRC equals
 * the recorded one; ICON_* == GICON_* for the new ids. Prints "FAIL <name>" per
 * failing check and exits 1. `icons_b_test --dump` prints fresh table rows.
 * Built with -DGFX_NCTX=2 and no fb.c, so it defines gfx_ctx_hook. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spinlock.h"
#include "icons.h"
#include "logit.h"

long ic_test_sys(long n, long a, long b, long c) { (void)n; (void)a; (void)b; (void)c; return 0; }

#define SAME(a, b) _Static_assert((a) == (b), #a " != " #b)
SAME(ICON_SHARE, GICON_SHARE);     SAME(ICON_PLAY, GICON_PLAY);
SAME(ICON_PAUSE, GICON_PAUSE);     SAME(ICON_TRASH, GICON_TRASH);
SAME(ICON_ARROW_L, GICON_ARROW_L); SAME(ICON_ARROW_R, GICON_ARROW_R);
SAME(ICON_ARROW_U, GICON_ARROW_U); SAME(ICON_ARROW_D, GICON_ARROW_D);
SAME(ICON_INFO, GICON_INFO);       SAME(ICON_WARNING, GICON_WARNING);
SAME(ICON_DOWNLOAD, GICON_DOWNLOAD); SAME(ICON_REFRESH, GICON_REFRESH);
SAME(ICON_STAR, GICON_STAR);       SAME(ICON_LOCK, GICON_LOCK);
SAME(ICON_HOME, GICON_HOME);       SAME(ICON_LINK, GICON_LINK);
SAME(ICON_EYE, GICON_EYE);         SAME(ICON_BELL, GICON_BELL);
SAME(ICON_COUNT, GICON_BELL + 1);
SAME(ICON_SHARE, ICON_GEAR + 1);   /* set B is appended: existing ids unchanged */

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
static uint32_t crc32(const uint8_t *p, int n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}

/* Recorded at 32 px, weight 2: ink = sum of coverage / 255 (pixel equivalents),
 * crc = CRC-32 of the 32x32 coverage bitmap. Regenerate with --dump. */
static const struct { const char *name; int id; long ink; uint32_t crc; } REC[] = {
    { "ICON_SHARE", ICON_SHARE, 171, 0x3ee36ca5u },
    { "ICON_PLAY", ICON_PLAY, 170, 0x0fec03a2u },
    { "ICON_PAUSE", ICON_PAUSE, 197, 0xfadf94e3u },
    { "ICON_TRASH", ICON_TRASH, 212, 0x8c750491u },
    { "ICON_ARROW_L", ICON_ARROW_L, 127, 0x602aed50u },
    { "ICON_ARROW_R", ICON_ARROW_R, 127, 0xb99f87b7u },
    { "ICON_ARROW_U", ICON_ARROW_U, 128, 0xaa9e97eau },
    { "ICON_ARROW_D", ICON_ARROW_D, 128, 0x59dc560au },
    { "ICON_INFO", ICON_INFO, 197, 0x5932209cu },
    { "ICON_WARNING", ICON_WARNING, 172, 0x9f51644au },
    { "ICON_DOWNLOAD", ICON_DOWNLOAD, 161, 0xc2ffedcdu },
    { "ICON_REFRESH", ICON_REFRESH, 126, 0x38b9439au },
    { "ICON_STAR", ICON_STAR, 204, 0xf297ffc4u },
    { "ICON_LOCK", ICON_LOCK, 269, 0xad5c7725u },
    { "ICON_HOME", ICON_HOME, 207, 0xa91e8d39u },
    { "ICON_LINK", ICON_LINK, 155, 0x50cf3afeu },
    { "ICON_EYE", ICON_EYE, 168, 0xf4235a5au },
    { "ICON_BELL", ICON_BELL, 211, 0xb7bd8574u },
};
#define NREC ((int)(sizeof REC / sizeof *REC))

static void test_ids(void)
{
    static const int px[] = { 12, 16, 32, 64, 15, 18, 20, 24 };
    int empty = 0, outside = 0, wrongsz = 0;
    for (int id = ICON_SHARE; id < ICON_COUNT; id++)
        for (unsigned k = 0; k < sizeof px / sizeof *px; k++) {
            g_w = g_h = 0;
            icon_draw(id, 0, 0, px[k], 0xFFFFFF);
            if (g_w != px[k] || g_h != px[k]) { wrongsz++; continue; }
            if (ink() == 0) empty++;
            if (!ring_clear()) outside++;
        }
    CHECK(NREC == ICON_COUNT - ICON_SHARE, "table-complete");
    CHECK(!wrongsz, "size"); CHECK(!empty, "nonempty"); CHECK(!outside, "box");
}

static int delta(const uint8_t *a, const uint8_t *b, int px, int vertical)
{
    int m = 0;
    for (int y = 0; y < px; y++) for (int x = 0; x < px; x++) {
        int sx = vertical ? x : px - 1 - x, sy = vertical ? px - 1 - y : y;
        int d = abs((int)a[y * px + x] - (int)b[sy * px + sx]);
        if (d > m) m = d;
    }
    return m;
}

static void test_mirror(void)
{
    static uint8_t A[64 * 64], B[64 * 64];
    static const int px[] = { 12, 16, 32, 64, 15, 18, 24 };
    int worst = 0;
    for (unsigned k = 0; k < sizeof px / sizeof *px; k++) {
        int n = px[k], d;
        icon_draw(ICON_ARROW_L, 0, 0, n, 1); memcpy(A, g_cov, (size_t)n * n);
        icon_draw(ICON_ARROW_R, 0, 0, n, 1); memcpy(B, g_cov, (size_t)n * n);
        d = delta(A, B, n, 0); if (d > worst) worst = d;
        icon_draw(ICON_ARROW_U, 0, 0, n, 1); memcpy(A, g_cov, (size_t)n * n);
        icon_draw(ICON_ARROW_D, 0, 0, n, 1); memcpy(B, g_cov, (size_t)n * n);
        d = delta(A, B, n, 1); if (d > worst) worst = d;
    }
    printf("info arrow mirror max delta %d/255\n", worst);
    CHECK(worst <= 1, "mirror");
}

static void test_record(int dump)
{
    int band = 0, crc = 0;
    for (int id = ICON_SHARE; id < ICON_COUNT; id++) {
        icon_draw(id, 0, 0, 32, 1);
        long a = ink() / 255;
        uint32_t c = crc32(g_cov, 32 * 32);
        if (dump) { printf("    { \"%d\", %d, %ld, 0x%08xu },\n", id, id, a, c); continue; }
        const typeof(REC[0]) *r = 0;
        for (int i = 0; i < NREC; i++) if (REC[i].id == id) r = &REC[i];
        if (!r) { band++; crc++; continue; }
        /* +-25% around the recorded area, never below 40 px-equivalents */
        if (a < r->ink * 3 / 4 || a > r->ink * 5 / 4 || a < 40) { printf("info %s ink %ld (rec %ld)\n", r->name, a, r->ink); band++; }
        if (c != r->crc) crc++;
    }
    if (dump) return;
    CHECK(!band, "ink-area"); CHECK(!crc, "crc");
}

/* The set-B ids must not disturb the A-side cache: same key, same bitmap. */
static void test_stable(void)
{
    icon_draw(ICON_STAR, 0, 0, 32, 1); uint32_t a = crc32(g_cov, 32 * 32);
    icon_draw(ICON_GEAR, 0, 0, 32, 1);
    icon_draw(ICON_STAR, 0, 0, 32, 1); CHECK(crc32(g_cov, 32 * 32) == a, "stable");
    icon_draw(ICON_COUNT, 0, 0, 32, 1); icon_draw(-1, 0, 0, 32, 1);          /* rejected, no crash */
    /* large sizes: the path scratch must hold every new icon at wm.c's ceiling */
    int refused = 0;
    for (int id = ICON_SHARE; id < ICON_COUNT; id++) { g_w = 0; icon_draw(id, 0, 0, 512, 1); if (g_w != 512 || ink() == 0) refused++; }
    CHECK(!refused, "large");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--dump")) { test_record(1); return 0; }
    test_ids();
    test_mirror();
    test_record(0);
    test_stable();
    if (!bad) printf("icons-symbols-b: ok\n");
    return bad;
}
