/* SPDX-License-Identifier: GPL-3.0-or-later */
/* app_icon_cache.h: LRU budget, distinct per-scale keys, BOX tile accuracy
 * against a double area average, and the publish protocol app_icons.c runs
 * (two interleaved contexts, duplicate publish, real threads). Built with
 * -DGFX_NCTX=2; does not link fb.c, so it defines gfx_ctx_hook itself. Prints
 * "FAIL <name>" per failing check and exits 1. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_icon_cache.h"

static __thread int t_ctx;
static int hook_fn(void) { return t_ctx; }
int (*gfx_ctx_hook)(void) = hook_fn;

static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }

static int bad;
#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s\n", name); bad = 1; } } while (0)

static void *mk(unsigned char tag, int px)
{
    unsigned char *t = malloc(aic_tile_bytes(px));
    memset(t, tag, aic_tile_bytes(px));
    return t;
}

static void test_lru_budget(void)
{
    struct aic c; void *ev[AIC_SLOTS]; unsigned n;
    unsigned tb = aic_tile_bytes(64);
    aic_init(&c, tb * 3 + 100);                    /* fits exactly 3 tiles */
    int ok = 1;
    for (int app = 0; app < 10; app++) {
        void *t = mk((unsigned char)app, 64);
        if (aic_publish(&c, app, 64, t, ev, &n) != 1) ok = 0;
        for (unsigned i = 0; i < n; i++) free(ev[i]);
        if (c.used_bytes > c.budget) ok = 0;
        if (aic_count(&c) > 3) ok = 0;
    }
    CHECK(ok, "lru-budget");
    CHECK(aic_find(&c, 9, 64) && aic_find(&c, 8, 64) && aic_find(&c, 7, 64) && !aic_find(&c, 6, 64), "lru-order");
    /* touching 7 makes 8 the victim */
    aic_find(&c, 7, 64);
    void *t = mk(77, 64);
    CHECK(aic_publish(&c, 20, 64, t, ev, &n) == 1 && n == 1, "lru-evict-one");
    for (unsigned i = 0; i < n; i++) free(ev[i]);
    CHECK(aic_find(&c, 7, 64) && !aic_find(&c, 9, 64), "lru-recency");
    /* a tile larger than the whole budget is refused, not cached */
    struct aic small; aic_init(&small, 1000);
    void *big = mk(1, 64);
    CHECK(aic_publish(&small, 0, 64, big, ev, &n) == -1 && aic_count(&small) == 0, "lru-oversize");
    free(big);
    CHECK(aic_publish(&small, 0, AIC_MAX_PX + 1, 0, ev, &n) == -1, "lru-maxpx");
    for (unsigned i = 0; i < AIC_SLOTS; i++) free(c.e[i].tile);
}

static void test_scale_keys(void)
{
    struct aic c; void *ev[AIC_SLOTS]; unsigned n;
    aic_init(&c, 24u * 1024 * 1024);
    int pxs[3] = { 48 * 100 / 100, 48 * 125 / 100, 48 * 150 / 100 };
    for (int i = 0; i < 3; i++) aic_publish(&c, 4, pxs[i], mk((unsigned char)(i + 1), pxs[i]), ev, &n);
    int ok = aic_count(&c) == 3;
    for (int i = 0; i < 3; i++) {
        const unsigned char *t = aic_find(&c, 4, pxs[i]);
        if (!t || t[0] != i + 1) ok = 0;
    }
    CHECK(pxs[0] != pxs[1] && pxs[1] != pxs[2] && ok, "scale-keys");
    CHECK(!aic_find(&c, 5, pxs[0]), "scale-keys-app");
    for (unsigned i = 0; i < AIC_SLOTS; i++) free(c.e[i].tile);
}

static int chan(const uint8_t *p, int k) { return p[k]; }

static void test_box_tile(void)
{
    enum { S = 1536, D = 64, K = S / D };
    uint8_t *src = malloc((size_t)S * S * 4);
    double *lum = malloc(sizeof(double) * S * S);
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        double cx = x - S / 2.0, cy = y - S / 2.0;
        double v = 0.5 + 0.5 * cos(M_PI * (cx * cx + cy * cy) / (double)S * 0.5);
        int g = (int)(v * 255.0 + 0.5);
        uint8_t *p = src + ((size_t)y * S + x) * 4;
        p[0] = p[1] = p[2] = (uint8_t)g; p[3] = 255; lum[(size_t)y * S + x] = g;
    }
    uint8_t *tile = aic_build_tile(src, S, S, D, t_alloc, t_free);
    CHECK(tile != 0, "box-tile-built");
    if (tile) {
        double maxerr = 0;
        int opaque = 1;
        for (int j = 0; j < D; j++) for (int i = 0; i < D; i++) {
            double sum = 0;
            for (int y = 0; y < K; y++) for (int x = 0; x < K; x++) sum += lum[(size_t)(j * K + y) * S + i * K + x];
            const uint8_t *p = tile + ((size_t)j * D + i) * 4;
            for (int q = 0; q < 3; q++) { double e = fabs(chan(p, q) - sum / (K * K)); if (e > maxerr) maxerr = e; }
            if (p[3] != 255) opaque = 0;
        }
        printf("box-tile max err %.3f/255\n", maxerr);
        CHECK(maxerr <= 2.0, "box-tile");
        CHECK(opaque, "box-tile-opaque");
        free(tile);
    }
    free(src); free(lum);

    /* alpha: constant colour, alpha ramps over x. The tile must keep the
     * straight colour and the area-averaged alpha. */
    enum { W = 256, H = 256, DD = 32, KK = W / DD };
    uint8_t *a = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = a + ((size_t)y * W + x) * 4;
        p[0] = 200; p[1] = 100; p[2] = 50; p[3] = (uint8_t)x;
    }
    uint8_t *ta = aic_build_tile(a, W, H, DD, t_alloc, t_free);
    CHECK(ta != 0, "box-alpha-built");
    if (ta) {
        int worst_a = 0, worst_c = 0;
        for (int i = 0; i < DD; i++) {
            double av = 0;
            for (int k = 0; k < KK; k++) av += i * KK + k;
            av /= KK;
            const uint8_t *p = ta + (size_t)i * 4;
            int da = abs((int)(p[3]) - (int)(av + 0.5));
            if (da > worst_a) worst_a = da;
            if (p[3] >= 64) {
                static const int c0[3] = { 200, 100, 50 };
                for (int q = 0; q < 3; q++) { int d = abs(p[q] - c0[q]); if (d > worst_c) worst_c = d; }
            }
        }
        printf("box-alpha max alpha err %d, colour err %d\n", worst_a, worst_c);
        CHECK(worst_a <= 2, "box-alpha");
        CHECK(worst_c <= 4, "box-alpha-colour");
        free(ta);
    }
    free(a);
}

/* ---- the miss / publish protocol of app_icon_draw ---- */
static struct aic g_cache;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_alloc, g_free;
static pthread_mutex_t g_cnt = PTHREAD_MUTEX_INITIALIZER;
static void *ctr_mk(unsigned char tag, int px) { pthread_mutex_lock(&g_cnt); g_alloc++; pthread_mutex_unlock(&g_cnt); return mk(tag, px); }
static void ctr_free(void *p) { pthread_mutex_lock(&g_cnt); g_free++; pthread_mutex_unlock(&g_cnt); free(p); }

/* Phase 1 of a draw: returns 1 on a hit (tile verified under the lock). */
static int draw_lookup(unsigned app, int px)
{
    pthread_mutex_lock(&g_lock);
    const unsigned char *t = aic_find(&g_cache, app, px);
    int hit = t != 0, okk = !t || (t[0] == (unsigned char)app && t[aic_tile_bytes(px) - 1] == (unsigned char)app);
    pthread_mutex_unlock(&g_lock);
    if (!okk) { printf("FAIL tile-torn\n"); bad = 1; }
    return hit;
}
/* Phase 2: publish a tile built outside the lock. */
static int draw_publish(unsigned app, int px, void *fresh)
{
    void *ev[AIC_SLOTS]; unsigned n;
    pthread_mutex_lock(&g_lock);
    int r = aic_publish(&g_cache, app, px, fresh, ev, &n);
    pthread_mutex_unlock(&g_lock);
    for (unsigned i = 0; i < n; i++) ctr_free(ev[i]);
    if (r != 1) ctr_free(fresh);
    return r;
}

static void test_interleaved_contexts(void)
{
    aic_init(&g_cache, 24u * 1024 * 1024);
    g_alloc = g_free = 0;
    /* ctx 0 and ctx 1 both miss the same key, both build, ctx 1 publishes
     * first, then ctx 0: exactly one tile, the loser's is freed. */
    t_ctx = 0; int m0 = !draw_lookup(3, 64);
    t_ctx = 1; int m1 = !draw_lookup(3, 64);
    void *f0 = (t_ctx = 0, ctr_mk(3, 64)), *f1 = (t_ctx = 1, ctr_mk(3, 64));
    t_ctx = 1; int r1 = draw_publish(3, 64, f1);
    t_ctx = 0; int r0 = draw_publish(3, 64, f0);
    CHECK(m0 && m1 && r1 == 1 && r0 == 0, "dup-publish");
    CHECK(aic_count(&g_cache) == 1 && g_alloc - g_free == 1, "dup-one-tile");
    t_ctx = 0; CHECK(draw_lookup(3, 64), "ctx-hit-0");
    t_ctx = 1; CHECK(draw_lookup(3, 64), "ctx-hit-1");
    /* distinct key from the other context stays independent */
    t_ctx = 1; CHECK(!draw_lookup(3, 80), "ctx-miss-other-px");
    for (unsigned i = 0; i < AIC_SLOTS; i++) if (g_cache.e[i].tile) free(g_cache.e[i].tile);
}

static void *worker(void *arg)
{
    t_ctx = (int)(long)arg;
    unsigned s = 12345u + (unsigned)t_ctx * 777u;
    for (int it = 0; it < 4000; it++) {
        s = s * 1664525u + 1013904223u;
        unsigned app = (s >> 16) % 8;
        int px = 32 + (int)((s >> 8) % 3) * 8;
        if (draw_lookup(app, px)) continue;
        draw_publish(app, px, ctr_mk((unsigned char)app, px));
    }
    return 0;
}

static void test_threads(void)
{
    aic_init(&g_cache, aic_tile_bytes(40) * 6);   /* far smaller than 24 keys: constant eviction */
    g_alloc = g_free = 0;
    pthread_t th[2];
    for (long i = 0; i < 2; i++) pthread_create(&th[i], 0, worker, (void *)i);
    for (int i = 0; i < 2; i++) pthread_join(th[i], 0);
    CHECK(g_cache.used_bytes <= g_cache.budget, "thread-budget");
    int resident = 0; unsigned sum = 0;
    for (unsigned i = 0; i < AIC_SLOTS; i++) if (g_cache.e[i].tile) { resident++; sum += g_cache.e[i].bytes; free(g_cache.e[i].tile); }
    CHECK(sum == g_cache.used_bytes, "thread-bytes");
    CHECK(g_alloc - g_free == resident, "thread-leak");
}

int main(void)
{
    test_lru_budget();
    test_scale_keys();
    test_box_tile();
    test_interleaved_contexts();
    test_threads();
    printf(bad ? "app_icon_cache: FAILED\n" : "app_icon_cache: ok\n");
    return bad ? 1 : 0;
}
